#include <Wire.h>
#include <Adafruit_PN532.h>
#include <avr/sleep.h>

#define PN532_IRQ   (2) // Obowiązkowo pin 2 w Arduino Nano dla przerwań sprzętowych
#define PN532_RESET (8) 

Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);

const int realyPin = 4; 
const int openingTime = 3000; 

// Flaga informująca, że przerwanie fizycznie wybudziło procesor
volatile bool wybudzonoPrzezNFC = false;

void awokeFunction() {
  sleep_disable(); 
  wybudzonoPrzezNFC = true;
}

void enterSleep() {
  Serial.println("Arduino idzie spac...");
  Serial.flush(); // Czekamy na pełne opróżnienie bufora UART
  delay(10);
  
  // 1. Bardzo ważne: Konfigurujemy PN532, aby wszedł w tryb niskiego poboru prądu 
  // i czekał na kartę/telefon, wystawiając impuls na pin IRQ w razie wykrycia.
  // Używamy zera jako timeoutu - to konfiguruje czytnik w tryb ciągłego czuwania.
  // nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, NULL, NULL, 0); 
  
  // Serial.println("Starting passive read for an ISO14443A Card ...");
  // if (!nfc.startPassiveTargetIDDetection(PN532_MIFARE_ISO14443A)) {
  //   Serial.println("No phone. Waiting...");
  // } else {
  //   Serial.println("Card already present.");
  //   return;
  // }
  nfc.startPassiveTargetIDDetection(PN532_MIFARE_ISO14443A);

  // 2. Przygotowanie konfiguracji snu AVR
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  sleep_enable();
  
  // Zmieniamy tryb na FALLING (zbocze opadające), co eliminuje pętlę zawieszenia na stanie LOW
  wybudzonoPrzezNFC = false;
  attachInterrupt(digitalPinToInterrupt(PN532_IRQ), awokeFunction, FALLING);
  
  // 3. W tym momencie procesor idzie spać i czeka na zbliżenie telefonu
  sleep_cpu();                         
  
  // ----------------------------------------------------
  // --- PROCESOR WYBUDZA SIĘ TUTAJ PO ZBLIŻENIU TELEFONU ---
  // ----------------------------------------------------
  
  detachInterrupt(digitalPinToInterrupt(PN532_IRQ)); 
  
  // 4. Restart i odświeżenie magistrali I2C po wybudzeniu, aby uniknąć zawieszenia biblioteki Wire
  Wire.begin(); 
  delay(50); // Czas na stabilizację sygnałów po wybudzeniu zegara mikrokontrolera
  
  Serial.println("Arduino wybudzone!");
}

void setup(void) {
  Serial.begin(115200);
  while (!Serial) delay(10); 

  Serial.println("System init...");

  // WŁĄCZENIE WEWNĘTRZNEGO PULL-UP NA PINIE PRZERWANIA
  pinMode(PN532_IRQ, INPUT_PULLUP); 

  pinMode(realyPin, OUTPUT);
  digitalWrite(realyPin, HIGH); 

  nfc.begin();

  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.print("No PN532. Check wires!");
    while (1); 
  }
  
  nfc.SAMConfig();
  Serial.println("\nSystem ready");
}

void loop(void) {
  // Ponieważ loop() kończy się wywołaniem enterSleep(), 
  // ten fragment kodu wykona się WYŁĄCZNIE wtedy, gdy telefon realnie wybudzi Arduino.
  
  if (wybudzonoPrzezNFC) {
    Serial.println("\nTelefon wykryty! Uruchamiam transmisje danych APDU...");
    
    // Ponowna inicjalizacja sesji w PN532 po przerwaniu snu
    nfc.SAMConfig(); 

    // Przełączenie PN532 w tryb gotowości do wymiany danych
    if (nfc.inListPassiveTarget()) {
      
      uint8_t selectApdu[] = { 0x00, 0xA4, 0x04, 0x00, 0x07, 0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x00 };
      uint8_t response[64];
      uint8_t responseLength = sizeof(response);

      bool success = nfc.inDataExchange(selectApdu, sizeof(selectApdu), response, &responseLength);
      
      if (success) {
        if (responseLength >= 2 && response[responseLength - 2] == 0x90 && response[responseLength - 1] == 0x00) {
          Serial.print("Sukces! Odebrano tekst: ");
          for (uint8_t i = 0; i < responseLength - 2; i++) {
            Serial.print((char)response[i]);
          }
          Serial.println();
          
          // --- URUCHOMIENIE PRZEKAŹNIKA ---
          Serial.println("Otwieram zamek...");
          digitalWrite(realyPin, LOW); 
          delay(openingTime);
          digitalWrite(realyPin, HIGH); 
          Serial.println("Zamek zamkniety.");
        }
      } else {
        Serial.println("Blad transakcji APDU (Handshake failed).");
      }
    }
    
    // Zapobiega natychmiastowemu ponownemu uśpieniu, dając użytkownikowi czas na zabranie telefonu
    delay(2000); 
  }

  // Wprowadzenie Arduino w stan czuwania. Program zatrzyma się wewnątrz tej funkcji,
  // dopóki ponowne zbliżenie urządzenia nie wywoła przerwania sprzętowego.
  enterSleep();
}
