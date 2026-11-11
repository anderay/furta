/*
  Arduino Nano + PN532 (I2C) + Android ISO-DEP/HCE example
  Direct PN532 host protocol; no NFC library is used.

  Wiring (classic ATmega328P Nano):
    Nano A4 (SDA) -> PN532 SDA
    Nano A5 (SCL) -> PN532 SCL
    Nano D2        <- PN532 IRQ (active LOW)
    GND            -> GND

  IMPORTANT:
  - Put the PN532 V3 module's interface switches/jumpers in I2C mode.
  - Check the module's voltage requirements/level shifting. Bare PN532 PVDD is max 3.6 V.
  - The NXP manual writes the address byte as 0x48/0x49. Arduino Wire uses
    the 7-bit address, therefore this sketch uses 0x24.
  - Serial output is for diagnostics and materially increases total power use.
*/

#include <Wire.h>
#include <avr/sleep.h>
#include <avr/interrupt.h>
#include <util/atomic.h>

static const uint8_t PN532_I2C_ADDR = 0x24; // 7-bit form of 0x48/0x49
static const uint8_t PN532_IRQ_PIN  = 2;    // INT0 on ATmega328P Nano

static const uint8_t PN532_HOST_TO_PN532 = 0xD4;
static const uint8_t PN532_PN532_TO_HOST = 0xD5;

static const uint8_t CMD_GET_GENERAL_STATUS  = 0x04;
static const uint8_t CMD_SAM_CONFIGURATION   = 0x14;
static const uint8_t CMD_POWER_DOWN          = 0x16;
static const uint8_t CMD_RF_CONFIGURATION    = 0x32;
static const uint8_t CMD_IN_DATA_EXCHANGE    = 0x40;
static const uint8_t CMD_IN_LIST_PASSIVE     = 0x4A;
static const uint8_t CMD_GET_FIRMWARE_VERSION = 0x02;

// PowerDown WakeUpEnable bits: I2C=0x80, RF level detector=0x08.
static const uint8_t WAKE_I2C_AND_RF = 0x88;

// Nano/Wire has a 32-byte receive buffer. This is enough for the short frames
// used here, but not for arbitrary long ATS or R-APDU data.
static const uint8_t I2C_READ_CHUNK = 32;
static const uint8_t RESPONSE_MAX = 27; // payload after D5 and response code
static const uint32_t I2C_TIMEOUT_US = 50000UL;

volatile bool irqWake = false;

void pn532IrqISR() {
  irqWake = true;
}

static void printHex(const uint8_t *data, uint8_t len) {
  for (uint8_t i = 0; i < len; ++i) {
    if (data[i] < 0x10) Serial.print('0');
    Serial.print(data[i], HEX);
    if (i + 1 < len) Serial.print(' ');
  }
  Serial.println();
}

static bool waitIrqLow(uint32_t timeoutMs) {
  uint32_t started = millis();
  while (digitalRead(PN532_IRQ_PIN) != LOW) {
    if ((uint32_t)(millis() - started) >= timeoutMs) return false;
  }
  return true;
}

// Send one normal PN532 information frame over I2C.
// 'body' starts with the command code and excludes TFI (D4).
static bool pn532WriteCommand(const uint8_t *body, uint8_t bodyLen) {
  if (bodyLen > 24) return false; // stay below the AVR Wire TX buffer

  const uint8_t len = bodyLen + 1; // include TFI D4
  const uint8_t lcs = (uint8_t)(0U - len);
  uint8_t sum = PN532_HOST_TO_PN532;

  Wire.beginTransmission(PN532_I2C_ADDR);
  Wire.write((uint8_t)0x00); // preamble
  Wire.write((uint8_t)0x00); // start code 1
  Wire.write((uint8_t)0xFF); // start code 2
  Wire.write(len);
  Wire.write(lcs);
  Wire.write(PN532_HOST_TO_PN532);
  for (uint8_t i = 0; i < bodyLen; ++i) {
    Wire.write(body[i]);
    sum = (uint8_t)(sum + body[i]);
  }
  Wire.write((uint8_t)(0U - sum)); // DCS
  Wire.write((uint8_t)0x00);       // postamble
  uint8_t error = Wire.endTransmission(true);
#if defined(WIRE_HAS_TIMEOUT)
  if (Wire.getWireTimeoutFlag()) {
    Wire.clearWireTimeoutFlag();
    return false;
  }
#endif
  return error == 0;
}

// Read one complete short PN532 frame. PN532 I2C prepends an RDY status byte.
// Returns frame bytes beginning at 00 00 FF, without the I2C status byte.
static int16_t pn532ReadFrame(uint8_t *frame, uint8_t frameCapacity) {
  uint8_t raw[I2C_READ_CHUNK];
  uint8_t got = Wire.requestFrom(PN532_I2C_ADDR, I2C_READ_CHUNK);
  if (got < 7) {
    while (Wire.available()) (void)Wire.read();
    return -1;
  }

  uint8_t n = 0;
  while (Wire.available() && n < I2C_READ_CHUNK) raw[n++] = Wire.read();
  if ((raw[0] & 0x01) == 0) return -2; // not ready

  // ACK is 00 00 FF 00 FF 00.
  if (n >= 7 && raw[1] == 0x00 && raw[2] == 0x00 && raw[3] == 0xFF &&
      raw[4] == 0x00 && raw[5] == 0xFF && raw[6] == 0x00) {
    if (frameCapacity < 6) return -3;
    for (uint8_t i = 0; i < 6; ++i) frame[i] = raw[i + 1];
    return 6;
  }

  if (n < 9 || raw[1] != 0x00 || raw[2] != 0x00 || raw[3] != 0xFF) return -4;
  uint8_t len = raw[4];
  uint8_t lcs = raw[5];
  if ((uint8_t)(len + lcs) != 0) return -5;

  // frame size = preamble/start(3) + LEN/LCS(2) + LEN data + DCS + postamble
  uint16_t total = (uint16_t)len + 7U;
  if (total > frameCapacity || total + 1U > n) return -6;

  uint8_t sum = 0;
  for (uint8_t i = 0; i < len; ++i) sum = (uint8_t)(sum + raw[6 + i]);
  if ((uint8_t)(sum + raw[6 + len]) != 0) return -7;

  for (uint8_t i = 0; i < total; ++i) frame[i] = raw[i + 1];
  return (int16_t)total;
}

// Complete PN532 command transaction: command -> ACK -> response.
// response receives bytes after D5 and response-command-code.
static bool pn532Command(const uint8_t *body, uint8_t bodyLen,
                         uint8_t expectedResponseCode,
                         uint8_t *response, uint8_t &responseLen,
                         uint32_t responseTimeoutMs) {
  responseLen = 0;
  uint8_t frame[I2C_READ_CHUNK];

  if (!pn532WriteCommand(body, bodyLen)) return false;

  if (!waitIrqLow(100)) return false;
  int16_t ackLen = pn532ReadFrame(frame, sizeof(frame));
  if (ackLen != 6) return false;

  if (!waitIrqLow(responseTimeoutMs)) return false;
  int16_t frameLen = pn532ReadFrame(frame, sizeof(frame));
  if (frameLen < 9) return false;

  const uint8_t dataLen = frame[3];
  // frame[4]=LCS, frame[5]=TFI, frame[6]=response code
  if (dataLen < 2) return false;
  if (frame[5] != PN532_PN532_TO_HOST || frame[6] != expectedResponseCode) return false;

  uint8_t payloadLen = dataLen - 2; // remove D5 and response code
  if (payloadLen > RESPONSE_MAX) return false;
  for (uint8_t i = 0; i < payloadLen; ++i) response[i] = frame[7 + i];
  responseLen = payloadLen;
  return true;
}

// Wake/probe the PN532 without waiting for IRQ. In LowVbat/PowerDown the
// PN532 may stretch SCL while its oscillator starts. Wire timeout prevents a
// permanently wedged setup if the board is not really in I2C mode.
static bool pn532WakeAndProbe() {
  Wire.beginTransmission(PN532_I2C_ADDR);
  uint8_t error = Wire.endTransmission(true);
#if defined(WIRE_HAS_TIMEOUT)
  if (Wire.getWireTimeoutFlag()) {
    Wire.clearWireTimeoutFlag();
    return false;
  }
#endif
  delay(5); // PN532 oscillator start is specified below 2 ms; add margin.
  return error == 0;
}

static bool samNormalMode() {
  const uint8_t cmd[] = { CMD_SAM_CONFIGURATION, 0x01, 0x00, 0x01 };
  uint8_t response[RESPONSE_MAX], responseLen;
  return pn532Command(cmd, sizeof(cmd), 0x15, response, responseLen, 500);
}

static bool configureFinitePollingRetries() {
  // RFConfiguration, CfgItem 05: MxRtyATR=0, MxRtyPSL=1,
  // MxRtyPassiveActivation=2. Avoids an infinite InListPassiveTarget wait.
  const uint8_t cmd[] = { CMD_RF_CONFIGURATION, 0x05, 0x00, 0x01, 0x02 };
  uint8_t response[RESPONSE_MAX], responseLen;
  return pn532Command(cmd, sizeof(cmd), 0x33, response, responseLen, 500);
}

static bool getGeneralStatus() {
  const uint8_t cmd[] = { CMD_GET_GENERAL_STATUS };
  uint8_t response[RESPONSE_MAX], responseLen;
  return pn532Command(cmd, sizeof(cmd), 0x05, response, responseLen, 500);
}

static bool enterPn532PowerDown() {
  // Wake on either an I2C command or external RF field. GenerateIRQ=1 makes
  // PN532 assert P70_IRQ after RF wake-up.
  const uint8_t cmd[] = { CMD_POWER_DOWN, WAKE_I2C_AND_RF, 0x01 };
  uint8_t response[RESPONSE_MAX], responseLen;
  if (!pn532Command(cmd, sizeof(cmd), 0x17, response, responseLen, 500)) return false;
  return responseLen >= 1 && (response[0] & 0x3F) == 0x00;
}

static bool listAndroidTarget(uint8_t &targetNumber) {
  // InListPassiveTarget(MaxTg=1, BrTy=0x00 = 106-kbit/s Type A)
  const uint8_t cmd[] = { CMD_IN_LIST_PASSIVE, 0x01, 0x00 };
  uint8_t response[RESPONSE_MAX], responseLen;
  if (!pn532Command(cmd, sizeof(cmd), 0x4B, response, responseLen, 1500)) return false;
  if (responseLen < 2 || response[0] == 0) return false;

  targetNumber = response[1];
  Serial.print(F("Target data: "));
  printHex(response, responseLen);
  return true;
}

static bool exchangeRequestedApdu(uint8_t targetNumber) {
  // Exact APDU requested by the application.
  const uint8_t apdu[] = { 0x0F, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
  uint8_t cmd[2 + sizeof(apdu)];
  cmd[0] = CMD_IN_DATA_EXCHANGE;
  cmd[1] = targetNumber;
  for (uint8_t i = 0; i < sizeof(apdu); ++i) cmd[2 + i] = apdu[i];

  uint8_t response[RESPONSE_MAX], responseLen;
  if (!pn532Command(cmd, sizeof(cmd), 0x41, response, responseLen, 2000)) return false;
  if (responseLen < 1) return false;

  const uint8_t status = response[0];
  if ((status & 0x3F) != 0) {
    Serial.print(F("InDataExchange PN532 status: 0x"));
    Serial.println(status, HEX);
    return false;
  }

  Serial.print(F("Android response APDU: "));
  printHex(response + 1, responseLen - 1);
  return true;
}

static void sleepArduinoUntilPn532Irq() {
  irqWake = false;

  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  sleep_enable();

  // Avoid the race where IRQ goes low between the test and SLEEP instruction.
  noInterrupts();
  if (digitalRead(PN532_IRQ_PIN) == HIGH) {
    // Brown-out remains enabled on a classic Nano for portability.
    interrupts();
    sleep_cpu();
  } else {
    interrupts();
  }
  sleep_disable();
}

static bool getFirmwareVersion()
{
  const uint8_t cmd[] = {
    CMD_GET_FIRMWARE_VERSION
  };

  uint8_t response[RESPONSE_MAX];
  uint8_t responseLen = 0;

  if (!pn532Command(
          cmd,
          sizeof(cmd),
          0x03,       // GetFirmwareVersion response code
          response,
          responseLen,
          1000)) {
    return false;
  }

  // Response:
  // IC, Ver, Rev, Support
  if (responseLen < 4) {
    return false;
  }

  Serial.print(F("PN532 IC: 0x"));
  Serial.println(response[0], HEX);

  Serial.print(F("Firmware: "));
  Serial.print(response[1], DEC);
  Serial.print('.');
  Serial.println(response[2], DEC);

  Serial.print(F("Support flags: 0x"));
  Serial.println(response[3], HEX);

  return true;
}

void setup()
{
  Serial.begin(115200);

  /*
   * Match Adafruit_PN532(2, 8):
   *
   * D2 is the real IRQ connection.
   * D8 is ignored because this PN532 V3 module does not expose a usable reset
   * connection.
   */
  pinMode(PN532_IRQ_PIN, INPUT);

  attachInterrupt(
      digitalPinToInterrupt(PN532_IRQ_PIN),
      pn532IrqISR,
      LOW);

  Wire.begin();
//  Wire.setClock(100000);

// #if defined(WIRE_HAS_TIMEOUT)
//   /*
//    * PN532 can stretch SCL while waking.
//    * This timeout is only protection against a permanently stuck bus.
//    */
//   Wire.setWireTimeout(200000UL, true);
// #endif

  delay(100);

  Serial.println(F("Sending SAMConfiguration(Normal)..."));

  if (!samNormalMode()) {
    Serial.println(F("SAMConfiguration failed."));
    return;
  }

  Serial.println(F("Sending GetFirmwareVersion..."));

  if (!getFirmwareVersion()) {
    Serial.println(F("GetFirmwareVersion failed."));
    return;
  }

  Serial.println(F("PN532 initialized."));

  if (!configureFinitePollingRetries()) {
    Serial.println(F("PN532 retry configuration failed."));
    return;
  }

  Serial.println(F("PN532 ready."));
}

void loop() {
  Serial.println(F("Entering PN532 + Arduino sleep; present Android phone..."));

  if (!enterPn532PowerDown()) {
    Serial.println(F("PowerDown failed; retrying"));
    delay(500);
    return;
  }

  // NXP recommends allowing about 1 ms after the PowerDown response.
  delay(2);
  sleepArduinoUntilPn532Irq();

  Serial.println(F("IRQ wake-up"));

  // The first command after RF wake releases P70_IRQ. GetGeneralStatus is a
  // convenient harmless command and confirms that the PN532 is responsive.
  if (!getGeneralStatus()) {
    Serial.println(F("GetGeneralStatus after wake failed"));
    delay(100);
    return;
  }

  uint8_t target = 0;
  if (!listAndroidTarget(target)) {
    Serial.println(F("No ISO14443-A target found"));
    delay(100);
    return;
  }

  if (!exchangeRequestedApdu(target)) {
    Serial.println(F("APDU exchange failed"));
  }

  delay(250); // let the phone/card leave or settle before sleeping again
}
