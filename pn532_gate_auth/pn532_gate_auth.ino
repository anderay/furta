/*
 * Arduino Nano + PN532 V3 (I2C) -> Android phone (HCE): GATE OPENER with phone enrollment
 *
 * Low-power polling (see pn532_android_apdu_lowpower.ino) + challenge-response
 * + phones that can be added/removed over NFC (the Arduino never needs re-flashing).
 *
 * Roles
 *   ADMIN phones ("master cards"): IDs listed in ADMIN_IDS (flash). They can open the gate
 *                and manage the user list. Provision them once with make_keys.py.
 *   USER phones: IDs stored in EEPROM (up to MAX_USERS). Added by an admin over NFC.
 *
 * Keys
 *   phoneKey(ID) = first 16 bytes of HMAC-SHA256(MASTER_KEY, ID)
 *   The gate stores only MASTER_KEY + the list of allowed IDs, never the phone keys.
 *
 * Protocol after the phone is in the field
 *   gate  -> phone : SELECT AID                          00 A4 04 00 07 <AID> 00
 *   gate  -> phone : AUTH challenge                      80 10 00 00 08 <challenge[8]>
 *   phone -> gate  : ID[2] CMD[1] ARG[2] MAC[8] 90 00    normal answer
 *                    6F 01                               "I want to join" (new phone)
 *   MAC = first 8 bytes of HMAC-SHA256(phoneKey, "GATE1" | challenge | ID | CMD | ARG)
 *   gate  -> phone : RESULT                              80 30 <code> 00   (phone shows feedback)
 *
 *   CMD 0 OPEN    open the gate (any allowed phone)
 *   CMD 1 ENROLL  admin: open the enrollment window (ENROLL_ITERATIONS polls, ~1 min)
 *   CMD 2 REMOVE  admin: remove user phone ARG
 *   CMD 3 CLEAR   admin: remove all user phones
 *
 * Adding a phone (2 taps)
 *   1. Admin app: "Add new device", tap the admin phone to the gate  -> window opens.
 *   2. New phone app: "Join gate", tap the new phone to the gate within the window.
 *      The gate picks the next free ID (never reused), pushes ID + phoneKey to the phone
 *      (80 20 IDhi IDlo 10 <key[16]>) and stores the ID in EEPROM. The window then closes.
 *   The key is sent over NFC in clear once, during the window (acceptable here, see notes).
 *
 * Challenge: no hardware RNG on the ATmega328P, so the challenge is
 * HMAC(GATE_SECRET, bootCounter | counter | micros | millis).
 *
 * Wiring: SDA->A4, SCL->A5, IRQ->D2, relay/transistor input -> D4 (GATE_PIN).
 * Do not drive a relay coil directly from the pin; use a transistor or relay module.
 *
 * Change MASTER_KEY, GATE_SECRET and ADMIN_IDS before use (python3 make_keys.py init).
 */

#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <avr/sleep.h>
#include <avr/wdt.h>
#include <avr/pgmspace.h>
#include <string.h>

// ---------------------------------------------------------------- config ----
#define DEBUG           1
#define DIAG_NO_POLL    0        // 1 = never poll, only sleep (measure the sleep floor)

#define PN532_IRQ_PIN   2        // INT0
#define PN532_I2C_ADDR  0x24     // 7-bit (manual: 0x48 write / 0x49 read)

// Watchdog periods: WDTO_15MS, 30MS, 60MS, 120MS, 250MS, 500MS, 1S, 2S, 4S, 8S
#define POLL_PERIOD     WDTO_500MS   // sleep between polls (= max detection latency)
#define POLL_TIMEOUT    WDTO_250MS   // max wait for the poll response
#define POLL_RETRIES    0x01         // MxRtyPassiveActivation (0xFF = infinite, avoid)
#define FAIL_LOCKOUT    WDTO_2S      // sleep after a failed authentication
#define OPEN_LOCKOUT    WDTO_4S      // sleep after opening (phone is probably still there)

#define GATE_PIN        4
#define GATE_ACTIVE     HIGH         // level that operates the gate relay
#define GATE_PULSE      WDTO_1S      // how long the relay is operated

static const uint8_t AID[] = { 0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };

// ----- SECRETS: replace with your own (python3 make_keys.py init) -----
static const uint8_t MASTER_KEY[32] = {
  0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
  0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
static const uint8_t GATE_SECRET[16] = {
  0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF };

// Admin phones (master cards). Must be < USER_ID_FIRST. Keep a spare admin ID in reserve.
static const uint16_t ADMIN_IDS[] = { 0x0001, 0x0002 };

#define ENROLL_ITERATIONS 120    // enrollment window in poll cycles (~60 s at 500 ms + overhead)
#define MAX_USERS         100    // user phones stored in EEPROM (2 bytes each)
#define USER_ID_FIRST     0x0100 // first ID handed out to user phones

// EEPROM layout
#define EE_BOOT           0      // uint32 boot counter
#define EE_MAGIC          4      // uint16 "initialised" marker
#define EE_NEXT_ID        6      // uint16 next ID to hand out (IDs are never reused)
#define EE_USERS          8      // uint16[MAX_USERS], 0xFFFF = empty slot
#define EE_MAGIC_VALUE    0xA55A

// Commands a phone can put into its AUTH answer
#define CMD_OPEN          0
#define CMD_ENROLL        1
#define CMD_REMOVE        2
#define CMD_CLEAR         3

// Result codes sent to the phone (80 30 <code> 00)
#define RES_OPENED        0x01
#define RES_ENROLL_OPEN   0x02
#define RES_REMOVED       0x03
#define RES_NOT_FOUND     0x04
#define RES_CLEARED       0x05
#define RES_NOT_OPEN      0x10   // phone wants to join but no enrollment window is open
#define RES_FULL          0x11
#define RES_DENIED        0xE0

// What the main loop does after a session
#define R_NONE            0
#define R_OPEN            1      // open the gate
#define R_FAIL            2      // failed attempt -> longer lockout

struct Sha256 {
  uint32_t h[8];
  uint8_t  buf[64];
  uint8_t  len;
  uint32_t total;
};

// ---------------------------------------------------------------- logging ---
#if DEBUG
  #define LOG(...)      Serial.print(__VA_ARGS__)
  #define LOGLN(...)    Serial.println(__VA_ARGS__)
  static void printHex(const uint8_t *d, uint8_t n) {
    for (uint8_t i = 0; i < n; i++) {
      if (d[i] < 0x10) Serial.print('0');
      Serial.print(d[i], HEX);
      Serial.print(' ');
    }
  }
  #define LOGHEX(d, n)  printHex((d), (n))
#else
  #define LOG(...)
  #define LOGLN(...)
  #define LOGHEX(d, n)
#endif

// ------------------------------------------------------- SHA-256 / HMAC -----
static const uint32_t K256[64] PROGMEM = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

static const uint32_t H256[8] PROGMEM = {
  0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };

// ------------------------------------------------------------ sleep ---------
ISR(WDT_vect) { }                       // must exist, otherwise the WDT interrupt resets the chip

static void irqIsr() {
  detachInterrupt(digitalPinToInterrupt(PN532_IRQ_PIN));   // level interrupt would retrigger
}

// Power-down sleep. Wakes on watchdog timeout (wdto = WDTO_xxx) and, if useIrq,
// also as soon as the PN532 pulls IRQ LOW.
static void sleepPD(uint8_t wdto, bool useIrq) {
#if DEBUG
  Serial.flush();                       // UART is off in power-down
#endif
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  noInterrupts();
  if (useIrq && digitalRead(PN532_IRQ_PIN) == LOW) {       // already ready, don't sleep
    interrupts();
    return;
  }
  if (useIrq) attachInterrupt(digitalPinToInterrupt(PN532_IRQ_PIN), irqIsr, LOW);

  wdt_reset();
  MCUSR &= ~_BV(WDRF);
  WDTCSR |= _BV(WDCE) | _BV(WDE);       // timed sequence: change WDT config
  WDTCSR = _BV(WDIE) | ((wdto & 0x08) ? _BV(WDP3) : 0) | (wdto & 0x07);   // interrupt mode only

  sleep_enable();
#ifdef sleep_bod_disable
  sleep_bod_disable();
#endif
  interrupts();                         // instruction after sei always executes
  sleep_cpu();                          // ---- sleeping ----
  sleep_disable();
  wdt_disable();
  if (useIrq) detachInterrupt(digitalPinToInterrupt(PN532_IRQ_PIN));
}

// Busy-wait (awake) until IRQ is LOW or timeout.
static bool waitIrqLow(uint16_t timeoutMs) {
  uint32_t t0 = millis();
  while (digitalRead(PN532_IRQ_PIN) == HIGH) {
    if (millis() - t0 >= timeoutMs) return false;
  }
  return true;
}

// ------------------------------------------------------- PN532 low level ----
// Frame: 00 00 FF LEN LCS TFI(D4) CMD DATA... DCS 00   (cmd = command code + params)
static bool pn532WriteFrame(const uint8_t *cmd, uint8_t cmdLen) {
  if (cmdLen == 0 || cmdLen > 24) return false;            // Wire buffer is 32 bytes
  const uint8_t len = cmdLen + 1;

  // The PN532 may be in power-down: it stretches SCL while waking, and can also NACK its
  // address right after finishing an exchange (manual 6.2.4) -> retry a few times.
  for (uint8_t attempt = 0; attempt < 5; attempt++) {
    uint8_t sum = 0xD4;
    Wire.beginTransmission(PN532_I2C_ADDR);
    Wire.write((uint8_t)0x00);
    Wire.write((uint8_t)0x00);
    Wire.write((uint8_t)0xFF);
    Wire.write(len);
    Wire.write((uint8_t)(~len + 1));
    Wire.write((uint8_t)0xD4);
    for (uint8_t i = 0; i < cmdLen; i++) {
      Wire.write(cmd[i]);
      sum += cmd[i];
    }
    Wire.write((uint8_t)(~sum + 1));
    Wire.write((uint8_t)0x00);
    if (Wire.endTransmission() == 0) return true;
    delay(3);
  }
  return false;
}

// Host ACK frame = abort the running command (PN532 sends no response; RF field goes off).
static void pn532Abort() {
  static const uint8_t ACK[6] = { 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00 };
  Wire.beginTransmission(PN532_I2C_ADDR);
  Wire.write(ACK, sizeof(ACK));
  Wire.endTransmission();
  delay(3);
}

static bool pn532ReadAck() {
  static const uint8_t ACK[6] = { 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00 };
  if (Wire.requestFrom((uint8_t)PN532_I2C_ADDR, (uint8_t)7) != 7) return false;
  bool ok = (Wire.read() & 0x01);                          // status byte, bit0 = RDY
  for (uint8_t i = 0; i < 6; i++) {
    if (Wire.read() != ACK[i]) ok = false;
  }
  return ok;
}

// Read one response frame -> payload (response code + data), length or <0 on error.
static int16_t pn532ReadFrame(uint8_t *out, uint8_t outMax) {
  uint8_t buf[32];
  uint8_t got = Wire.requestFrom((uint8_t)PN532_I2C_ADDR, (uint8_t)32);
  if (got < 9) return -1;
  for (uint8_t i = 0; i < got; i++) buf[i] = Wire.read();
  if (!(buf[0] & 0x01)) return -2;

  uint8_t i = 1;                                           // find start code 00 FF
  while (i + 1 < got && !(buf[i] == 0x00 && buf[i + 1] == 0xFF)) i++;
  if (i + 4 >= got) return -3;

  const uint8_t len = buf[i + 2];
  const uint8_t lcs = buf[i + 3];
  if ((uint8_t)(len + lcs) != 0 || len == 0) return -3;
  if (i + 4 + len >= got) return -4;

  uint8_t sum = 0;
  for (uint8_t k = 0; k < len; k++) sum += buf[i + 4 + k];
  sum += buf[i + 4 + len];
  if (sum != 0) return -5;

  if (buf[i + 4] == 0x7F) return -6;                       // application-level error frame
  if (buf[i + 4] != 0xD5) return -7;

  const uint8_t plen = len - 1;
  if (plen > outMax) return -8;
  memcpy(out, &buf[i + 5], plen);
  return plen;
}

// Write command and read its ACK.
static bool pn532Send(const uint8_t *cmd, uint8_t cmdLen) {
  if (!pn532WriteFrame(cmd, cmdLen)) return false;
  if (!waitIrqLow(100)) return false;                      // ACK (includes wake-up time)
  return pn532ReadAck();
}

// Read and validate the response to command code 'cmdCode'.
static int16_t pn532Receive(uint8_t *resp, uint8_t respMax, uint8_t cmdCode) {
  int16_t n = pn532ReadFrame(resp, respMax);
  delay(2);                                                // PN532 needs ~1 ms to re-enter power-down
  if (n < 1) return (n < 0) ? n : -14;
  if (resp[0] != (uint8_t)(cmdCode + 1)) return -15;
  return n;
}

// Send + wait (awake) + receive. For short commands.
static int16_t pn532Command(const uint8_t *cmd, uint8_t cmdLen,
                            uint8_t *resp, uint8_t respMax, uint16_t timeoutMs) {
  if (!pn532Send(cmd, cmdLen))       return -10;
  if (!waitIrqLow(timeoutMs))        return -13;
  return pn532Receive(resp, respMax, cmd[0]);
}

// InDataExchange: returns number of data bytes (without status), <0 on error.
static int16_t pn532DataExchange(uint8_t tg, const uint8_t *apdu, uint8_t apduLen,
                                 uint8_t *out, uint8_t outMax) {
  if (apduLen > 22) return -20;
  uint8_t cmd[24];
  cmd[0] = 0x40;
  cmd[1] = tg;
  memcpy(&cmd[2], apdu, apduLen);

  uint8_t resp[32];
  int16_t n = pn532Command(cmd, apduLen + 2, resp, sizeof(resp), 1000);
  if (n < 2) return (n < 0) ? n : -21;

  uint8_t status = resp[1] & 0x3F;                         // bit7 NAD, bit6 MI, bits0-5 error
  if (status != 0) {
    LOG(F("PN532 status error 0x")); LOGLN(status, HEX);
    return -22;
  }
  n -= 2;
  if (n > outMax) return -23;
  memcpy(out, &resp[2], n);
  return n;
}

// RFConfiguration item 1: switch the RF field explicitly (bit0 = on, auto-RFCA off).
// We do NOT rely on the PN532 switching the field off by itself after a poll.
static void pn532RfField(bool on) {
  uint8_t r[8];
  const uint8_t cmd[] = { 0x32, 0x01, (uint8_t)(on ? 0x01 : 0x00) };
  pn532Command(cmd, sizeof(cmd), r, sizeof(r), 100);
}

// One short poll. Returns logical target number (>0) if a phone answered, else 0.
static uint8_t pollForPhone() {
  uint8_t resp[32];
  const uint8_t cmd[] = { 0x4A, 0x01, 0x00 };              // InListPassiveTarget, 1 target, 106 kbps A

  pn532RfField(true);                                      // explicit field on
  if (!pn532Send(cmd, sizeof(cmd))) {
    LOGLN(F("poll: send failed"));
    pn532RfField(false);
    return 0;
  }
  sleepPD(POLL_TIMEOUT, true);                             // wake on IRQ or timeout

  if (digitalRead(PN532_IRQ_PIN) == HIGH) {                // no response in time
    pn532Abort();
    LOGLN(F("poll: timeout, aborted"));
    pn532RfField(false);
    return 0;
  }
  int16_t n = pn532Receive(resp, sizeof(resp), cmd[0]);
  if (n < 3 || resp[1] == 0) {
    LOGLN(F("poll: no target"));
    pn532RfField(false);
    return 0;
  }                     // NbTg = 0 -> nobody there

  // resp: 4B NbTg Tg SENS_RES(2) SEL_RES NFCIDLen NFCID...
  LOG(F("Phone found, UID: "));
  if (n >= 7 && resp[6] <= n - 7) LOGHEX(&resp[7], resp[6]);
  LOGLN();
  return resp[2];
}


static inline uint32_t rotr32(uint32_t x, uint8_t n) { return (x >> n) | (x << (32 - n)); }

static void sha256Init(Sha256 *s) {
  for (uint8_t i = 0; i < 8; i++) s->h[i] = pgm_read_dword(&H256[i]);
  s->len = 0;
  s->total = 0;
}

static void sha256Block(Sha256 *s, const uint8_t *p) {
  uint32_t w[16];                                   // rolling message schedule
  uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
  uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];

  for (uint8_t t = 0; t < 64; t++) {
    if (t < 16) {
      w[t] = ((uint32_t)p[4 * t] << 24) | ((uint32_t)p[4 * t + 1] << 16) |
             ((uint32_t)p[4 * t + 2] << 8) | p[4 * t + 3];
    } else {
      uint32_t w15 = w[(t + 1) & 15], w2 = w[(t + 14) & 15];
      uint32_t s0 = rotr32(w15, 7) ^ rotr32(w15, 18) ^ (w15 >> 3);
      uint32_t s1 = rotr32(w2, 17) ^ rotr32(w2, 19) ^ (w2 >> 10);
      w[t & 15] += s0 + w[(t + 9) & 15] + s1;
    }
    uint32_t t1 = h + (rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25)) + ((e & f) ^ (~e & g)) +
                  pgm_read_dword(&K256[t]) + w[t & 15];
    uint32_t t2 = (rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
  s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256Update(Sha256 *s, const uint8_t *d, uint8_t n) {
  s->total += n;
  for (uint8_t i = 0; i < n; i++) {
    s->buf[s->len++] = d[i];
    if (s->len == 64) { sha256Block(s, s->buf); s->len = 0; }
  }
}

static void sha256Final(Sha256 *s, uint8_t out[32]) {
  uint32_t bits = s->total << 3;                    // messages here are tiny
  s->buf[s->len++] = 0x80;
  if (s->len > 56) {
    while (s->len < 64) s->buf[s->len++] = 0;
    sha256Block(s, s->buf);
    s->len = 0;
  }
  while (s->len < 56) s->buf[s->len++] = 0;
  s->buf[56] = s->buf[57] = s->buf[58] = s->buf[59] = 0;   // high 32 bits of the bit length
  s->buf[60] = bits >> 24; s->buf[61] = bits >> 16; s->buf[62] = bits >> 8; s->buf[63] = bits;
  sha256Block(s, s->buf);
  for (uint8_t i = 0; i < 8; i++) {
    out[4 * i]     = s->h[i] >> 24;
    out[4 * i + 1] = s->h[i] >> 16;
    out[4 * i + 2] = s->h[i] >> 8;
    out[4 * i + 3] = s->h[i];
  }
}

// HMAC-SHA256, key up to 64 bytes, message up to 255 bytes.
static void hmacSha256(const uint8_t *key, uint8_t keyLen,
                       const uint8_t *msg, uint8_t msgLen, uint8_t out[32]) {
  uint8_t k[64];
  uint8_t inner[32];
  Sha256 s;
  memset(k, 0, sizeof(k));
  memcpy(k, key, keyLen);

  for (uint8_t i = 0; i < 64; i++) k[i] ^= 0x36;
  sha256Init(&s);
  sha256Update(&s, k, 64);
  sha256Update(&s, msg, msgLen);
  sha256Final(&s, inner);

  for (uint8_t i = 0; i < 64; i++) k[i] ^= (0x36 ^ 0x5C);
  sha256Init(&s);
  sha256Update(&s, k, 64);
  sha256Update(&s, inner, 32);
  sha256Final(&s, out);
}

// ------------------------------------------------------------- EEPROM ------
static uint32_t bootCounter;        // incremented once per power-up
static uint32_t challengeCounter;   // incremented for every challenge (RAM)
static uint8_t  enrollLeft;         // remaining poll cycles of the enrollment window (RAM)

static uint16_t eeRead16(int addr) { uint16_t v; EEPROM.get(addr, v); return v; }
static void eeWrite16(int addr, uint16_t v) { EEPROM.put(addr, v); }   // put() only writes changed bytes

static void eepromInit() {
  if (eeRead16(EE_MAGIC) != EE_MAGIC_VALUE) {          // first run: empty user list
    for (uint8_t i = 0; i < MAX_USERS; i++) eeWrite16(EE_USERS + 2 * i, 0xFFFF);
    eeWrite16(EE_NEXT_ID, USER_ID_FIRST);
    eeWrite16(EE_MAGIC, EE_MAGIC_VALUE);
  }
}

static bool isAdmin(uint16_t id) {
  for (uint8_t i = 0; i < sizeof(ADMIN_IDS) / sizeof(ADMIN_IDS[0]); i++) {
    if (ADMIN_IDS[i] == id) return true;
  }
  return false;
}

static int16_t userSlot(uint16_t id) {                 // slot index or -1
  for (uint8_t i = 0; i < MAX_USERS; i++) {
    if (eeRead16(EE_USERS + 2 * i) == id) return i;
  }
  return -1;
}

static bool userExists(uint16_t id) { return id != 0xFFFF && userSlot(id) >= 0; }
static bool hasFreeSlot()           { return userSlot(0xFFFF) >= 0; }

static bool userAdd(uint16_t id) {
  int16_t slot = userSlot(0xFFFF);
  if (slot < 0) return false;
  eeWrite16(EE_USERS + 2 * slot, id);
  return true;
}

static bool userRemove(uint16_t id) {
  if (id == 0xFFFF) return false;
  int16_t slot = userSlot(id);
  if (slot < 0) return false;
  eeWrite16(EE_USERS + 2 * slot, 0xFFFF);
  return true;
}

static void usersClear() {
  for (uint8_t i = 0; i < MAX_USERS; i++) eeWrite16(EE_USERS + 2 * i, 0xFFFF);
}

// ------------------------------------------------------------ gate auth -----
static bool ctEqual(const uint8_t *a, const uint8_t *b, uint8_t n) {
  uint8_t diff = 0;
  for (uint8_t i = 0; i < n; i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

static void makeChallenge(uint8_t out[8]) {
  uint8_t seed[16];
  uint32_t m = micros(), t = millis();
  challengeCounter++;
  memcpy(&seed[0],  &bootCounter,      4);
  memcpy(&seed[4],  &challengeCounter, 4);
  memcpy(&seed[8],  &m,                4);
  memcpy(&seed[12], &t,                4);
  uint8_t h[32];
  hmacSha256(GATE_SECRET, sizeof(GATE_SECRET), seed, sizeof(seed), h);
  memcpy(out, h, 8);
}

// phoneKey(ID) = first 16 bytes of HMAC-SHA256(MASTER_KEY, ID)
static void deriveKey(uint16_t id, uint8_t key[16]) {
  const uint8_t idb[2] = { (uint8_t)(id >> 8), (uint8_t)id };
  uint8_t full[32];
  hmacSha256(MASTER_KEY, sizeof(MASTER_KEY), idb, 2, full);
  memcpy(key, full, 16);
}

// Expected MAC for (id, cmd, arg, challenge) -> out (8 bytes)
static void computeMac(uint16_t id, uint8_t cmd, uint16_t arg,
                       const uint8_t *challenge, uint8_t out[8]) {
  uint8_t key[16];
  deriveKey(id, key);

  uint8_t msg[5 + 8 + 2 + 1 + 2] = { 'G', 'A', 'T', 'E', '1' };
  memcpy(&msg[5], challenge, 8);
  msg[13] = id >> 8;  msg[14] = id;
  msg[15] = cmd;
  msg[16] = arg >> 8; msg[17] = arg;

  uint8_t full[32];
  hmacSha256(key, 16, msg, sizeof(msg), full);
  memcpy(out, full, 8);
}

static void sendResult(uint8_t tg, uint8_t code) {
  const uint8_t apdu[4] = { 0x80, 0x30, code, 0x00 };
  uint8_t data[8];
  pn532DataExchange(tg, apdu, sizeof(apdu), data, sizeof(data));
}

// A new phone asked to join. Only works while the enrollment window is open.
static uint8_t handleJoin(uint8_t tg) {
  if (enrollLeft == 0) {
    LOGLN(F("join request, window closed"));
    sendResult(tg, RES_NOT_OPEN);
    return R_NONE;
  }
  uint16_t id = eeRead16(EE_NEXT_ID);
  if (!hasFreeSlot() || id >= 0xFFF0) {
    LOGLN(F("join request, list full"));
    sendResult(tg, RES_FULL);
    return R_NONE;
  }

  // 80 20 IDhi IDlo 10 <key[16]>  (key is cleared from RAM afterwards)
  uint8_t apdu[21] = { 0x80, 0x20, (uint8_t)(id >> 8), (uint8_t)id, 16 };
  deriveKey(id, &apdu[5]);
  uint8_t data[8];
  int16_t k = pn532DataExchange(tg, apdu, sizeof(apdu), data, sizeof(data));
  memset(apdu, 0, sizeof(apdu));

  if (k == 2 && data[0] == 0x90 && data[1] == 0x00) {
    userAdd(id);
    eeWrite16(EE_NEXT_ID, id + 1);
    enrollLeft = 0;                                    // one phone per window
    LOG(F("phone enrolled, id 0x")); LOGLN(id, HEX);
  } else {
    LOG(F("phone refused the key, k=")); LOGLN(k);     // window stays open
  }
  return R_NONE;
}

// Everything that happens while a phone is in the field.
static uint8_t session(uint8_t tg) {
  uint8_t data[32];

  // 1) SELECT AID: 00 A4 04 00 Lc AID 00
  uint8_t sel[5 + sizeof(AID) + 1];
  sel[0] = 0x00; sel[1] = 0xA4; sel[2] = 0x04; sel[3] = 0x00;
  sel[4] = sizeof(AID);
  memcpy(&sel[5], AID, sizeof(AID));
  sel[5 + sizeof(AID)] = 0x00;

  int16_t m = pn532DataExchange(tg, sel, sizeof(sel), data, sizeof(data));
  if (m < 2 || data[m - 2] != 0x90 || data[m - 1] != 0x00) {
    LOG(F("SELECT failed: ")); LOGLN(m);
    return R_FAIL;
  }

  // 2) AUTH challenge: 80 10 00 00 08 <challenge[8]>
  uint8_t apdu[13] = { 0x80, 0x10, 0x00, 0x00, 0x08 };
  makeChallenge(&apdu[5]);
  int16_t k = pn532DataExchange(tg, apdu, sizeof(apdu), data, sizeof(data));

  if (k == 2 && data[0] == 0x6F && data[1] == 0x01) return handleJoin(tg);   // new phone

  // answer: ID[2] CMD ARG[2] MAC[8] 90 00
  if (k != 15 || data[13] != 0x90 || data[14] != 0x00) {
    LOG(F("AUTH bad answer: ")); LOGLN(k);
    return R_FAIL;
  }
  const uint16_t id  = ((uint16_t)data[0] << 8) | data[1];
  const uint8_t  cmd = data[2];
  const uint16_t arg = ((uint16_t)data[3] << 8) | data[4];

  uint8_t expected[8];
  computeMac(id, cmd, arg, &apdu[5], expected);
  const bool macOk   = ctEqual(expected, &data[5], 8);
  const bool admin   = isAdmin(id);
  const bool allowed = admin || userExists(id);

  if (!macOk || !allowed) {
    LOG(F("denied, phone 0x")); LOG(id, HEX); LOGLN(macOk ? F(" (not in list)") : F(" (bad MAC)"));
    sendResult(tg, RES_DENIED);
    return R_FAIL;
  }

  LOG(F("auth OK, phone 0x")); LOG(id, HEX); LOG(F(" cmd ")); LOGLN(cmd);

  if (cmd == CMD_OPEN) {
    sendResult(tg, RES_OPENED);
    return R_OPEN;
  }
  if (!admin) {                                        // all other commands are admin only
    sendResult(tg, RES_DENIED);
    return R_FAIL;
  }
  switch (cmd) {
    case CMD_ENROLL:
      enrollLeft = ENROLL_ITERATIONS;
      LOGLN(F("enrollment window open"));
      sendResult(tg, RES_ENROLL_OPEN);
      return R_NONE;
    case CMD_REMOVE:
      if (isAdmin(arg)) { sendResult(tg, RES_DENIED); return R_FAIL; }   // admins can't be removed
      sendResult(tg, userRemove(arg) ? RES_REMOVED : RES_NOT_FOUND);
      return R_NONE;
    case CMD_CLEAR:
      usersClear();
      sendResult(tg, RES_CLEARED);
      return R_NONE;
    default:
      sendResult(tg, RES_DENIED);
      return R_FAIL;
  }
}

static void openGate() {
  LOGLN(F("OPEN"));
  digitalWrite(GATE_PIN, GATE_ACTIVE);
  sleepPD(GATE_PULSE, false);                              // pin keeps its level in power-down
  digitalWrite(GATE_PIN, !GATE_ACTIVE);
}

// ---------------------------------------------------------------- setup -----
void setup() {
  pinMode(GATE_PIN, OUTPUT);
  digitalWrite(GATE_PIN, !GATE_ACTIVE);                    // gate closed first of all
#if DEBUG
  Serial.begin(115200);
#endif
  ADCSRA &= ~_BV(ADEN);                                    // ADC off (saves current in sleep)
  pinMode(PN532_IRQ_PIN, INPUT);

  EEPROM.get(EE_BOOT, bootCounter);                        // blank EEPROM reads 0xFFFFFFFF
  bootCounter++;
  EEPROM.put(EE_BOOT, bootCounter);
  eepromInit();

  Wire.begin();
  Wire.setClock(100000UL);
#if defined(WIRE_HAS_TIMEOUT)
  Wire.setWireTimeout(5000, true);                         // don't hang if SCL stays stretched
#endif
  delay(100);                                              // PN532 boot

  uint8_t r[32];
  int16_t n;

  const uint8_t cmdSam[] = { 0x14, 0x01, 0x14, 0x01 };     // SAMConfiguration: normal mode, use IRQ
  n = pn532Command(cmdSam, sizeof(cmdSam), r, sizeof(r), 500);
  if (n < 1) {
    LOG(F("PN532 not responding, err=")); LOGLN(n);
    while (1) { delay(1000); }
  }

  const uint8_t cmdFw[] = { 0x02 };                        // GetFirmwareVersion
  n = pn532Command(cmdFw, sizeof(cmdFw), r, sizeof(r), 200);
  if (n >= 5) {
    LOG(F("PN532 IC=0x")); LOG(r[1], HEX);
    LOG(F(" FW="));        LOG(r[2]); LOG('.'); LOGLN(r[3]);
  }

  const uint8_t cmdRf[] = { 0x32, 0x05, 0x02, 0x01, POLL_RETRIES };   // finite retries
  n = pn532Command(cmdRf, sizeof(cmdRf), r, sizeof(r), 200);
  if (n < 1) LOGLN(F("RFConfiguration failed"));

  const uint8_t cmdPm[] = { 0x08, 0x02, 0xFC, 0x02 };      // idle CPU mode = Power Down
  n = pn532Command(cmdPm, sizeof(cmdPm), r, sizeof(r), 200);
  if (n < 1) LOGLN(F("WriteRegister (power mode) failed"));

  const uint8_t cmdRd[] = { 0x06, 0x02, 0xFC };            // read it back
  n = pn532Command(cmdRd, sizeof(cmdRd), r, sizeof(r), 200);
  if (n >= 2) { LOG(F("idle power mode = 0x")); LOGLN(r[1], HEX); }
  else        { LOG(F("ReadRegister failed, err=")); LOGLN(n); }

  LOG(F("boot #")); LOGLN(bootCounter);
  LOGLN(F("Ready, polling."));
}

// ----------------------------------------------------------------- loop -----
void loop() {
  sleepPD(POLL_PERIOD, false);                             // both chips idle here
  if (enrollLeft) enrollLeft--;                            // enrollment window runs out

#if DIAG_NO_POLL
  return;
#endif
  uint8_t tg = pollForPhone();
  if (tg == 0) return;

  uint8_t res = session(tg);

  uint8_t r[32];
  const uint8_t cmdRel[] = { 0x52, 0x00 };                 // InRelease
  pn532Command(cmdRel, sizeof(cmdRel), r, sizeof(r), 200);
  pn532RfField(false);                                     // field off before the long sleeps

  if (res == R_OPEN) {
    openGate();
    sleepPD(OPEN_LOCKOUT, false);
  } else if (res == R_FAIL) {
    sleepPD(FAIL_LOCKOUT, false);
  } else {
    sleepPD(WDTO_1S, false);
  }
}
