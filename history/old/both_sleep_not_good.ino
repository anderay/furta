/*
 * Arduino Nano (ATmega328P) + PN532 V3 (I2C) -> Android phone (HCE), LOW-POWER polling
 *
 * Idea: poll briefly, sleep the rest of the time (both chips).
 *
 *   ATmega : SLEEP_MODE_PWR_DOWN, woken by the watchdog timer every POLL_PERIOD
 *   PN532  : idle CPU mode set to Power Down once at startup (bCPUPowerMode, XRAM 0x02FC,
 *            UM0701-02 section 3.1.3.2). The PN532 then wakes by itself when it sees its
 *            I2C address (it stretches SCL until its oscillator runs: typ. 0.5 ms, max 2 ms)
 *            and falls asleep again ~1 ms after each response.
 *   RF     : InListPassiveTarget with a finite retry count. When the retries run out the
 *            PN532 switches the RF field off by itself (UM0701-02 section 3.1.3.10).
 *
 * Wiring (same as before): SDA->A4, SCL->A5, IRQ->D2 (INT0), VCC->5V, GND->GND.
 *
 * Per cycle:
 *   sleep (watchdog) -> InListPassiveTarget -> sleep until IRQ or timeout
 *     no phone : back to sleep
 *     phone    : SELECT AID -> request APDU -> InRelease -> cooldown sleep
 *
 * NOT VERIFIED ON HARDWARE. Things to check on first run (watch Serial, DEBUG=1):
 *   - "idle power mode = 0x02" is printed at startup (WriteRegister accepted)
 *   - POLL_RETRIES: see UM0701-02 section 7.3.1 (RFConfiguration item 5) for what a
 *     "retry" costs in time, then tune POLL_RETRIES / POLL_TIMEOUT.
 * For current measurements set DEBUG to 0 (UART activity keeps the ATmega awake longer).
 */

#include <Arduino.h>
#include <Wire.h>
#include <avr/sleep.h>
#include <avr/wdt.h>
#include <string.h>

// ---------------------------------------------------------------- config ----
#define DEBUG           1

#define PN532_IRQ_PIN   2        // INT0
#define PN532_I2C_ADDR  0x24     // 7-bit (manual: 0x48 write / 0x49 read)

// Watchdog periods: WDTO_15MS, 30MS, 60MS, 120MS, 250MS, 500MS, 1S, 2S, 4S, 8S
#define POLL_PERIOD     WDTO_500MS   // sleep between polls (= max detection latency)
#define POLL_TIMEOUT    WDTO_250MS   // max wait for the poll response
#define COOLDOWN        WDTO_1S      // sleep after a finished exchange
#define POLL_RETRIES    0x01         // MxRtyPassiveActivation (0xFF = infinite, avoid)

#define USE_SELECT_AID  1            // Android HCE needs SELECT AID before your APDUs
static const uint8_t AID[]      = { 0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
static const uint8_t APP_APDU[] = { 0x0F, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };

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

// ---------------------------------------------------------- application -----
// One short poll. Returns logical target number (>0) if a phone answered, else 0.
static uint8_t pollForPhone() {
  uint8_t resp[32];
  const uint8_t cmd[] = { 0x4A, 0x01, 0x00 };              // InListPassiveTarget, 1 target, 106 kbps A

  if (!pn532Send(cmd, sizeof(cmd))) {
    LOGLN(F("poll: send failed"));
    return 0;
  }
  sleepPD(POLL_TIMEOUT, true);                             // wake on IRQ or timeout

  if (digitalRead(PN532_IRQ_PIN) == HIGH) {                // no response in time
    pn532Abort();                                          // also switches the RF field off
    return 0;
  }
  int16_t n = pn532Receive(resp, sizeof(resp), cmd[0]);
  if (n < 3 || resp[1] == 0) return 0;                     // NbTg = 0 -> nobody there

  // resp: 4B NbTg Tg SENS_RES(2) SEL_RES NFCIDLen NFCID...
  LOG(F("Phone found, UID: "));
  if (n >= 7 && resp[6] <= n - 7) LOGHEX(&resp[7], resp[6]);
  LOGLN();
  return resp[2];
}

static void talkToPhone(uint8_t tg) {
  uint8_t data[32];

#if USE_SELECT_AID
  uint8_t sel[5 + sizeof(AID) + 1];                        // 00 A4 04 00 Lc AID 00
  sel[0] = 0x00; sel[1] = 0xA4; sel[2] = 0x04; sel[3] = 0x00;
  sel[4] = sizeof(AID);
  memcpy(&sel[5], AID, sizeof(AID));
  sel[5 + sizeof(AID)] = 0x00;

  int16_t m = pn532DataExchange(tg, sel, sizeof(sel), data, sizeof(data));
  LOG(F("SELECT AID -> "));
  if (m < 0) { LOG(F("error ")); LOGLN(m); return; }
  LOGHEX(data, m); LOGLN();
  if (m < 2 || data[m - 2] != 0x90 || data[m - 1] != 0x00) {
    LOGLN(F("AID not accepted (expected ... 90 00)"));
    return;
  }
#endif

  int16_t k = pn532DataExchange(tg, APP_APDU, sizeof(APP_APDU), data, sizeof(data));
  LOG(F("Request -> "));
  if (k < 0) { LOG(F("error ")); LOGLN(k); return; }
  LOG(k); LOG(F(" bytes: "));
  LOGHEX(data, k); LOGLN();
}

// ---------------------------------------------------------------- setup -----
void setup() {
#if DEBUG
  Serial.begin(115200);
#endif
  ADCSRA &= ~_BV(ADEN);                                    // ADC off (saves current in sleep)
  pinMode(PN532_IRQ_PIN, INPUT);
  Wire.begin();
  Wire.setClock(100000UL);
#if defined(WIRE_HAS_TIMEOUT)
  Wire.setWireTimeout(5000, true);                         // don't hang if SCL stays stretched
#endif
  delay(100);                                              // PN532 boot

  uint8_t r[32];
  int16_t n;

  // After power-up the PN532 is in LowVbat mode; SAMConfiguration(normal) leaves it
  // (manual 3.1.3.3). IRQ byte = 1: PN532 drives P70_IRQ.
  const uint8_t cmdSam[] = { 0x14, 0x01, 0x14, 0x01 };
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

  // RFConfiguration item 5: MxRtyATR, MxRtyPSL, MxRtyPassiveActivation (all finite)
  const uint8_t cmdRf[] = { 0x32, 0x05, 0x02, 0x01, POLL_RETRIES };
  n = pn532Command(cmdRf, sizeof(cmdRf), r, sizeof(r), 200);
  if (n < 1) LOGLN(F("RFConfiguration failed"));

  // Idle CPU mode -> Power Down: WriteRegister XRAM 0x02FC = 0x02 (manual 3.1.3.2)
  const uint8_t cmdPm[] = { 0x08, 0x02, 0xFC, 0x02 };
  n = pn532Command(cmdPm, sizeof(cmdPm), r, sizeof(r), 200);
  if (n < 1) LOGLN(F("WriteRegister (power mode) failed"));

  // Read it back. This is also the first command sent to a (hopefully) sleeping PN532,
  // so it tests the wake-up path.
  const uint8_t cmdRd[] = { 0x06, 0x02, 0xFC };
  n = pn532Command(cmdRd, sizeof(cmdRd), r, sizeof(r), 200);
  if (n >= 2) { LOG(F("idle power mode = 0x")); LOGLN(r[1], HEX); }
  else        { LOG(F("ReadRegister failed, err=")); LOGLN(n); }

  LOGLN(F("Ready, polling."));
}

// ----------------------------------------------------------------- loop -----
void loop() {
  sleepPD(POLL_PERIOD, false);                             // both chips idle here

  uint8_t tg = pollForPhone();
  if (tg == 0) return;

  talkToPhone(tg);

  uint8_t r[32];
  const uint8_t cmdRel[] = { 0x52, 0x00 };                 // InRelease (RF field goes off)
  pn532Command(cmdRel, sizeof(cmdRel), r, sizeof(r), 200);

  // const uint8_t cmdRfOff[] = { 0x32, 0x01, 0x00 };         // RFConfiguration, field off
  // pn532Command(cmdRfOff, sizeof(cmdRfOff), r, sizeof(r), 200);

  sleepPD(COOLDOWN, false);                                // give the phone time to leave
}
