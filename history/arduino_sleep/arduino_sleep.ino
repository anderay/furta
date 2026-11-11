/*
 * Arduino Nano (ATmega328P) + PN532 NFC module V3 (I2C) -> Android phone (HCE)
 *
 * - No NFC library, raw PN532 frames over I2C (Wire only)
 * - InListPassiveTarget (0x4A) + InDataExchange (0x40)
 * - Arduino sleeps in POWER_DOWN; PN532 P70_IRQ (active LOW) wakes it on INT0 (D2)
 *
 * Wiring (PN532 V3 board set to I2C mode via its SEL0/SEL1 switches - check the
 * table printed on your board; Elechouse V3: SEL0=ON(1), SEL1=OFF(0)):
 *   PN532 VCC -> Nano 5V
 *   PN532 GND -> Nano GND
 *   PN532  
 *   PN532 SCL -> Nano A5
 *   PN532 IRQ -> Nano D2   (INT0, can wake from power-down on LOW level)
 *
 * Flow:
 *   1. SAMConfiguration, RFConfiguration (infinite passive-activation retries)
 *   2. InListPassiveTarget  -> PN532 polls on its own, Arduino sleeps
 *   3. IRQ goes LOW when the phone is found -> Arduino wakes
 *   4. (optional) SELECT AID, needed by Android HCE to route APDUs to your service
 *   5. InDataExchange with APDU 0F 01 02 03 04 05 06, print the answer
 *   6. InRelease, short cooldown, back to 2.
 */

#include <Arduino.h>
#include <Wire.h>
#include <avr/sleep.h>
#include <string.h>

// ---------------------------------------------------------------- config ----
#define PN532_IRQ_PIN   2        // INT0
#define PN532_I2C_ADDR  0x24     // 7-bit address (0x48 >> 1)
#define COOLDOWN_MS     1500

// Android HCE only forwards APDUs to your service after a successful SELECT AID.
// The AID must match <aid-filter> in the Android app's apduservice.xml.
#define USE_SELECT_AID  1
static const uint8_t AID[] = { 0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };

// The APDU requested from the phone
static const uint8_t APP_APDU[] = { 0x0F, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };

// ---------------------------------------------------------- IRQ + sleep -----
static void irqIsr() {
  detachInterrupt(digitalPinToInterrupt(PN532_IRQ_PIN));  // LOW level would retrigger
}

// Wait (polling) until IRQ is LOW or timeout.
static bool waitIrqLow(uint16_t timeoutMs) {
  uint32_t t0 = millis();
  while (digitalRead(PN532_IRQ_PIN) == HIGH) {
    if (millis() - t0 >= timeoutMs) return false;
  }
  return true;
}

// Sleep in power-down until PN532 pulls IRQ LOW.
static void sleepUntilIrq() {
  Serial.flush();                       // UART is off in power-down
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  noInterrupts();
  if (digitalRead(PN532_IRQ_PIN) == HIGH) {   // not ready yet -> sleep
    attachInterrupt(digitalPinToInterrupt(PN532_IRQ_PIN), irqIsr, LOW);
    sleep_enable();
#ifdef sleep_bod_disable
    sleep_bod_disable();
#endif
    interrupts();                       // the instruction after sei() always runs
    sleep_cpu();                        // -> sleeps here, wakes on INT0 LOW
    sleep_disable();
  } else {
    interrupts();                       // already ready, don't sleep
  }
  detachInterrupt(digitalPinToInterrupt(PN532_IRQ_PIN));
}

// ------------------------------------------------------- PN532 low level ----
// Frame: 00 00 FF LEN LCS TFI(D4) CMD DATA... DCS 00
// 'cmd' = command code + parameters (without TFI). Max 24 bytes (Wire buffer = 32).
static bool pn532WriteFrame(const uint8_t *cmd, uint8_t cmdLen) {
  if (cmdLen == 0 || cmdLen > 24) return false;
  const uint8_t len = cmdLen + 1;                 // TFI + payload

  // PN532 may be asleep: the first address byte wakes it and can be NACKed -> retry
  for (uint8_t attempt = 0; attempt < 5; attempt++) {
    uint8_t sum = 0xD4;
    Wire.beginTransmission(PN532_I2C_ADDR);
    Wire.write((uint8_t)0x00);
    Wire.write((uint8_t)0x00);
    Wire.write((uint8_t)0xFF);
    Wire.write(len);
    Wire.write((uint8_t)(~len + 1));              // LCS
    Wire.write((uint8_t)0xD4);                    // TFI host -> PN532
    for (uint8_t i = 0; i < cmdLen; i++) {
      Wire.write(cmd[i]);
      sum += cmd[i];
    }
    Wire.write((uint8_t)(~sum + 1));              // DCS
    Wire.write((uint8_t)0x00);
    if (Wire.endTransmission() == 0) return true;
    delay(3);
  }
  return false;
}

// Read the 6-byte ACK frame (preceded by the I2C status byte).
static bool pn532ReadAck() {
  static const uint8_t ACK[6] = { 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00 };
  if (Wire.requestFrom((uint8_t)PN532_I2C_ADDR, (uint8_t)7) != 7) return false;
  bool ok = (Wire.read() & 0x01);                 // status byte: bit0 = ready
  for (uint8_t i = 0; i < 6; i++) {
    if (Wire.read() != ACK[i]) ok = false;
  }
  return ok;
}

// Read a response frame, return payload length (response code + data), <0 on error.
static int16_t pn532ReadFrame(uint8_t *out, uint8_t outMax) {
  uint8_t buf[32];
  uint8_t got = Wire.requestFrom((uint8_t)PN532_I2C_ADDR, (uint8_t)32);
  if (got < 9) return -1;
  for (uint8_t i = 0; i < got; i++) buf[i] = Wire.read();
  if (!(buf[0] & 0x01)) return -2;                // status byte: not ready

  uint8_t i = 1;                                  // find start code 00 FF
  while (i + 1 < got && !(buf[i] == 0x00 && buf[i + 1] == 0xFF)) i++;
  if (i + 4 >= got) return -3;

  const uint8_t len = buf[i + 2];
  const uint8_t lcs = buf[i + 3];
  if ((uint8_t)(len + lcs) != 0 || len == 0) return -3;
  if (i + 4 + len >= got) return -4;              // frame doesn't fit in what we read

  uint8_t sum = 0;
  for (uint8_t k = 0; k < len; k++) sum += buf[i + 4 + k];   // TFI + payload
  sum += buf[i + 4 + len];                                    // + DCS
  if (sum != 0) return -5;

  if (buf[i + 4] == 0x7F) return -6;              // application-level error frame
  if (buf[i + 4] != 0xD5) return -7;              // TFI PN532 -> host

  const uint8_t plen = len - 1;
  if (plen > outMax) return -8;
  memcpy(out, &buf[i + 5], plen);
  return plen;
}

// Send command, check ACK, wait for response (polling or sleeping on IRQ).
static int16_t pn532Command(const uint8_t *cmd, uint8_t cmdLen,
                            uint8_t *resp, uint8_t respMax,
                            uint16_t timeoutMs, bool sleepForResponse = false) {
  if (!pn532WriteFrame(cmd, cmdLen)) return -10;
  if (!waitIrqLow(100))              return -11;
  if (!pn532ReadAck())               return -12;

  if (sleepForResponse) sleepUntilIrq();
  else if (!waitIrqLow(timeoutMs))   return -13;

  int16_t n = pn532ReadFrame(resp, respMax);
  if (n < 1) return (n < 0) ? n : -14;
  if (resp[0] != (uint8_t)(cmd[0] + 1)) return -15;
  return n;
}

// ------------------------------------------------------------- helpers ------
static void printHex(const uint8_t *d, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) {
    if (d[i] < 0x10) Serial.print('0');
    Serial.print(d[i], HEX);
    Serial.print(' ');
  }
}

// InDataExchange: returns number of data bytes in 'out', negative on error.
static int16_t pn532DataExchange(uint8_t tg, const uint8_t *apdu, uint8_t apduLen,
                                 uint8_t *out, uint8_t outMax) {
  if (apduLen > 22) return -20;
  uint8_t cmd[24];
  cmd[0] = 0x40;                                  // InDataExchange
  cmd[1] = tg;
  memcpy(&cmd[2], apdu, apduLen);

  uint8_t resp[32];
  int16_t n = pn532Command(cmd, apduLen + 2, resp, sizeof(resp), 1000);
  if (n < 2) return (n < 0) ? n : -21;

  uint8_t status = resp[1] & 0x3F;                // 0x00 = OK, else PN532 error code
  if (status != 0) {
    Serial.print(F("PN532 status error 0x"));
    Serial.println(status, HEX);
    return -22;
  }
  // (bit 6 of resp[1] = MI "more information"; not handled in this simple example)
  n -= 2;
  if (n > outMax) return -23;
  memcpy(out, &resp[2], n);
  return n;
}

// -------------------------------------------------------------- setup -------
void setup() {
  Serial.begin(115200);
  pinMode(PN532_IRQ_PIN, INPUT);
  Wire.begin();
  Wire.setClock(100000UL);
  delay(200);                                     // PN532 boot time

  uint8_t r[32];

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // GetFirmwareVersion (0x02)
  const uint8_t cmdFw[] = { 0x02 };
  int16_t n = pn532Command(cmdFw, sizeof(cmdFw), r, sizeof(r), 200);
  if (n < 5) {
    Serial.print(F("PN532 not found, err="));
    Serial.println(n);
    while (1) { delay(1000); }
  }
  Serial.print(F("PN532 IC=0x")); Serial.print(r[1], HEX);
  Serial.print(F(" FW="));        Serial.print(r[2]);
  Serial.print('.');              Serial.println(r[3]);

  // SAMConfiguration (0x14): normal mode, timeout 0x14 (unused), use IRQ pin
  const uint8_t cmdSam[] = { 0x14, 0x01, 0x14, 0x01 };
  n = pn532Command(cmdSam, sizeof(cmdSam), r, sizeof(r), 200);
  if (n < 1) { Serial.println(F("SAMConfiguration failed")); while (1) {} }

  // RFConfiguration (0x32), item 5 = MaxRetries:
  //   MxRtyATR=0xFF, MxRtyPSL=0x01, MxRtyPassiveActivation=0xFF (retry forever)
  // -> InListPassiveTarget stays pending until a target shows up (IRQ then goes LOW)
  const uint8_t cmdRf[] = { 0x32, 0x05, 0xFF, 0x01, 0xFF };
  n = pn532Command(cmdRf, sizeof(cmdRf), r, sizeof(r), 200);
  if (n < 1) { Serial.println(F("RFConfiguration failed")); while (1) {} }

  Serial.println(F("Ready."));
}

// --------------------------------------------------------------- loop -------
void loop() {
  uint8_t resp[32];
  bool approved {false};

  // 1) InListPassiveTarget: MaxTg=1, 106 kbps ISO14443 type A.
  //    Arduino sleeps until the PN532 pulls IRQ LOW (= response ready).
  const uint8_t cmdList[] = { 0x4A, 0x01, 0x00 };
  Serial.println(F("Waiting for phone (sleeping)..."));
  int16_t n = pn532Command(cmdList, sizeof(cmdList), resp, sizeof(resp), 0, true);
  Serial.println(F("Woke up"));

  // resp: 4B NbTg Tg SENS_RES(2) SEL_RES NFCIDLen NFCID... [ATS...]
  if (n < 8 || resp[1] == 0) {
    Serial.print(F("No target / error: "));
    Serial.println(n);
    delay(COOLDOWN_MS);
    return;
  }
  const uint8_t tg      = resp[2];
  const uint8_t selRes  = resp[5];
  const uint8_t uidLen  = resp[6];
  Serial.print(F("Target ")); Serial.print(tg);
  Serial.print(F(", SEL_RES=0x")); Serial.print(selRes, HEX);
  Serial.print(F(", UID: "));
  if (uidLen <= n - 7) printHex(&resp[7], uidLen);
  Serial.println();

  uint8_t data[32];

#if USE_SELECT_AID
  // 2) SELECT AID: 00 A4 04 00 Lc AID 00
  uint8_t sel[5 + sizeof(AID) + 1];
  sel[0] = 0x00; sel[1] = 0xA4; sel[2] = 0x04; sel[3] = 0x00;
  sel[4] = sizeof(AID);
  memcpy(&sel[5], AID, sizeof(AID));
  sel[5 + sizeof(AID)] = 0x00;

  int16_t m = pn532DataExchange(tg, sel, sizeof(sel), data, sizeof(data));
  Serial.print(F("SELECT AID -> "));
  if (m < 0) {
    Serial.print(F("error ")); Serial.println(m);
    goto release;
  }
  printHex(data, m); Serial.println();
  if (m < 2 || data[m - 2] != 0x90 || data[m - 1] != 0x00) {
    Serial.println(F("AID not accepted by the phone (expect ... 90 00)"));
    goto release;
  }
#endif

  approved = true;
  // {
  //   // 3) The actual request: APDU 0F 01 02 03 04 05 06
  //   int16_t k = pn532DataExchange(tg, APP_APDU, sizeof(APP_APDU), data, sizeof(data));
  //   Serial.print(F("Request -> "));
  //   if (k < 0) {
  //     Serial.print(F("error ")); Serial.println(k);
  //   } else {
  //     Serial.print(F("Response (")); Serial.print(k); Serial.print(F(" bytes): "));
  //     printHex(data, k);
  //     Serial.println();
  //   }
  // }

release:
  // 4) InRelease (0x52), Tg=0 -> release all targets
  {
    const uint8_t cmdRel[] = { 0x52, 0x00 };
    pn532Command(cmdRel, sizeof(cmdRel), resp, sizeof(resp), 200);
  }
  // 5) RFConfiguration, field off
  {
    const uint8_t cmdRfOff[] = { 0x32, 0x01, 0x00 };
    pn532Command(cmdRfOff, sizeof(cmdRfOff), resp, sizeof(resp), 200);
  }

  if (approved) {
    digitalWrite(LED_BUILTIN, HIGH);
    delay(1000);
    digitalWrite(LED_BUILTIN, LOW);
  }

  delay(COOLDOWN_MS);
}
