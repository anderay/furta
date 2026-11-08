#include <Wire.h>
#include <Adafruit_PN532.h>
#include <avr/sleep.h> // Biblioteka do zarządzania snem procesora

// Definicja pinów dla połączenia I2C w Arduino Nano: A4 (SDA), A5 (SCL)
#define PN532_IRQ   (2) // Można zostawić niepodłączony, biblioteka wymaga definicji
#define PN532_RESET (8) // Można zostawić niepodłączony

// Inicjalizacja modułu PN532 w trybie I2C
Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);

// Konfiguracja pinu przekaźnika
const int realyPin = 4; 
const int openingTime = 3000; // Czas w milisekundach (3 sekundy)

// --- TUTAJ WPISZ ODREJESTROWANY UID TWOJEGO TELEFONU ---
// Przykładowo, jeśli Twój Samsung wysyła 4-bajtowy kod: 0x1A, 0x2B, 0x3C, 0x4D
// Jeśli usługa FIDO wysyła kod 7-bajtowy, tablica po prostu zapełni się do 7 pozycji.
// const byte zaufany_UID[] = {0x1A, 0x2B, 0x3C, 0x4D}; 
// const int dlugosc_zaufanego_UID = sizeof(zaufany_UID);

// Flaga informująca, że przerwanie fizycznie wybudziło procesor
volatile bool wybudzonoPrzezNFC = false;

void awokeFunction() {
  sleep_disable(); 
  wybudzonoPrzezNFC = true;
}

void enterSleep() {
  Serial.println("Arduino enters deep sleep...");
  Serial.flush(); // Czekamy na pełne opróżnienie bufora UART
  delay(10);

  // 1. Bardzo ważne: Konfigurujemy PN532, aby wszedł w tryb niskiego poboru prądu 
  // i czekał na kartę/telefon, wystawiając impuls na pin IRQ w razie wykrycia.
  // Używamy zera jako timeoutu - to konfiguruje czytnik w tryb ciągłego czuwania.
  nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, NULL, NULL, 0); 

  set_sleep_mode(SLEEP_MODE_PWR_DOWN); // Ustawienie najgłębszego trybu oszczędzania energii
  sleep_enable();                      // Autoryzacja przejścia w sen

  // Zmieniamy tryb na FALLING (zbocze opadające), co eliminuje pętlę zawieszenia na stanie LOW
  wybudzonoPrzezNFC = false;
  attachInterrupt(digitalPinToInterrupt(PN532_IRQ), awokeFunction, FALLING);

  sleep_cpu();                         // W tym momencie procesor zasypia i czeka na telefon...
  
  // --- PROCESOR WYBUDZA SIĘ TUTAJ PO ZBLIŻENIU TELEFONU ---
  
  detachInterrupt(digitalPinToInterrupt(PN532_IRQ)); // Odpięcie przerwania na czas procesowania danych

  // 4. Restart i odświeżenie magistrali I2C po wybudzeniu, aby uniknąć zawieszenia biblioteki Wire
  Wire.begin(); 
  delay(50); // Czas na stabilizację sygnałów po wybudzeniu zegara mikrokontrolera

  Serial.println("Arduino woken!");
}

void setup(void) {
  Serial.begin(115200);
  while (!Serial) delay(10); // Oczekiwanie na otwarcie portu szeregowego

  // Konfiguracja pinu przekaźnika jako wyjście
  pinMode(realyPin, OUTPUT);
  digitalWrite(realyPin, HIGH); // Dla większości modułów przekaźników HIGH = wyłączony (stan spoczynku)

  nfc.begin();

  uint32_t versiondata = nfc.getFirmwareVersion();
  if (! versiondata) {
    Serial.print("No PN532. Check wires!");
    while (1); // Zablokuj program w przypadku błędu
  }
  
  // Wyświetlenie informacji o znalezionym chipie
  Serial.print("PN5X2 found");
  
  // Konfiguracja PN532 do odczytu tagów RFID
  nfc.SAMConfig();
  
  Serial.println("\nSystem ready");
}

void loop(void) {

  // 1. Configure PN532 to look for an ISO14443A card capable of data exchange
  // This explicitly prepares the internal state machine for inDataExchange.
  bool success = nfc.inListPassiveTarget();
  
  if (success) {
    Serial.println("\nPhone detected via ISO14443A! Sending AID handshake...");

    // 2. Prepare APDU command to SELECT your specific AID (F0 01 02 03 04 05 06)
    uint8_t selectApdu[] = { 0x00, 0xA4, 0x04, 0x00, 0x07, 0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x00 };
  
    uint8_t response[64];
    uint8_t responseLength = sizeof(response);

    // 3. Send command to the phone
    // Note: The Adafruit library implementation of inDataExchange handles the current target handle internally
    success = nfc.inDataExchange(selectApdu, sizeof(selectApdu), response, &responseLength);
    
    if (success) {
      Serial.print("Data received (Raw Hex): ");
      for (uint8_t i = 0; i < responseLength; i++) {
        Serial.print(" 0x");
        if (response[i] < 0x10) Serial.print("0");
        Serial.print(response[i], HEX);
      }
      Serial.println();

      // Your app returns "BADGE123456" followed by 0x90 0x00 (13 bytes total)
      if (responseLength >= 2) {
        // Verify that the last two bytes are success status words (0x90 0x00)
        if (response[responseLength - 2] == 0x90 && response[responseLength - 1] == 0x00) {
          Serial.print("Success! Plaintext response: ");
          // Print only the text part, excluding the 0x90 0x00 bytes
          for (uint8_t i = 0; i < responseLength - 2; i++) {
            Serial.print((char)response[i]);
          }
          Serial.println();
          
          // --- Trigger Relay Here ---
          // digitalWrite(realyPin, LOW); // Open lock
          // delay(openingTime);
          // digitalWrite(realyPin, HIGH); // Close lock
        }
      }
    } else {
      Serial.println("Handshake failed. The phone dropped the connection or AID is unregistered.");
    }
  } else {
      Serial.println("Passive target list failed");
  }

  // Zapobiega natychmiastowemu ponownemu uśpieniu, dając użytkownikowi czas na zabranie telefonu
  delay(2000);
}

// void loop(void) {

//   Serial.println("In loop");

//   if (wybudzonoPrzezNFC) {
//     Serial.println("\Phone detected! Enable APDU transmission...");

//     // Ponowna inicjalizacja sesji w PN532 po przerwaniu snu
//     nfc.SAMConfig();

//     // 1. Configure PN532 to look for an ISO14443A card capable of data exchange
//     // This explicitly prepares the internal state machine for inDataExchange.
//     bool success = nfc.inListPassiveTarget();
    
//     if (success) {
//       Serial.println("\nPhone detected via ISO14443A! Sending AID handshake...");

//       // 2. Prepare APDU command to SELECT your specific AID (F0 01 02 03 04 05 06)
//       uint8_t selectApdu[] = { 0x00, 0xA4, 0x04, 0x00, 0x07, 0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x00 };
    
//       uint8_t response[64];
//       uint8_t responseLength = sizeof(response);

//       // 3. Send command to the phone
//       // Note: The Adafruit library implementation of inDataExchange handles the current target handle internally
//       success = nfc.inDataExchange(selectApdu, sizeof(selectApdu), response, &responseLength);
      
//       if (success) {
//         Serial.print("Data received (Raw Hex): ");
//         for (uint8_t i = 0; i < responseLength; i++) {
//           Serial.print(" 0x");
//           if (response[i] < 0x10) Serial.print("0");
//           Serial.print(response[i], HEX);
//         }
//         Serial.println();

//         // Your app returns "BADGE123456" followed by 0x90 0x00 (13 bytes total)
//         if (responseLength >= 2) {
//           // Verify that the last two bytes are success status words (0x90 0x00)
//           if (response[responseLength - 2] == 0x90 && response[responseLength - 1] == 0x00) {
//             Serial.print("Success! Plaintext response: ");
//             // Print only the text part, excluding the 0x90 0x00 bytes
//             for (uint8_t i = 0; i < responseLength - 2; i++) {
//               Serial.print((char)response[i]);
//             }
//             Serial.println();
            
//             // --- Trigger Relay Here ---
//             // digitalWrite(realyPin, LOW); // Open lock
//             // delay(openingTime);
//             // digitalWrite(realyPin, HIGH); // Close lock
//           }
//         }
//       } else {
//         Serial.println("Handshake failed. The phone dropped the connection or AID is unregistered.");
//       }
//     } else {
//         Serial.println("Passive target list failed");
//     }

//     // Zapobiega natychmiastowemu ponownemu uśpieniu, dając użytkownikowi czas na zabranie telefonu
//     delay(2000); 
//   }

//   // Wprowadzenie Arduino w stan czuwania. Program zatrzyma się wewnątrz tej funkcji,
//   // dopóki ponowne zbliżenie urządzenia nie wywoła przerwania sprzętowego.
//   enterSleep();
// }

// void loop(void) {
//   uint8_t success;
//   uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };  // Buffer to store the returned UID
//   uint8_t uidLength;                        // Length of the UID
  
//   // 1. Wait for an ISO14443A target (the Android Phone)
//   success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength);
  
//   if (success) {
//     for (uint8_t i=0; i < uidLength; i++) {
//       Serial.print(" 0x");
//       if (uid[i] < 0x10) Serial.print("0");
//       Serial.print(uid[i], HEX);
//     }
//     Serial.println("\nPhone detected! Sending AID handshake...");

//     // 2. Prepare APDU command to SELECT your specific AID (F0 01 02 03 04 05 06)
//     // Format: [CLA, INS, P1, P2, Lc, ...Data (AID)...]
//     // uint8_t selectApdu[] = { 0x00, 0xA4, 0x04, 0x00, 0x07, 0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
//     // uint8_t selectApdu[] = { 0x00, 0x04, 0x04, 0x00, 0x08, 0xA0, 0x00, 0x00, 0x06, 0x47, 0x2F, 0x00, 0x01 };
//     // uint8_t selectApdu[] = {
//     //   0x00, /* CLA - Klasa instrukcji */
//     //   0xA4, /* INS - Instrukcja: Wybór pliku/aplikacji */
//     //   0x04, /* P1  - Wybór według nazwy (AID) */
//     //   0x00, /* P2  - Pierwszy lub jedyny rekord */
//     //   // 0x07, /* Lc  - Długość danych AID (7 bajtów) */
//     //   // 0xA0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10 /* AID Portfela Android */
//     //   // 0x08,
//     //   // 0xA0, 0x00, 0x00, 0x06, 0x47, 0x2F, 0x00, 0x01
//     //   // 0x07,
//     //   // 0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01
//     //   0x03,
//     //   0xF0, 0x10, 0x10
//     // };
//     uint8_t selectApdu[] = { 0x00, 0xA4, 0x04, 0x00, 0x07, 0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x00 };
  
//     uint8_t response[64];
//     uint8_t responseLength = sizeof(response);

//     // 3. Send command to the phone
//     success = nfc.inDataExchange(selectApdu, sizeof(selectApdu), response, &responseLength);
    
//     if (success) {
//       Serial.print("Unique Android ID received: ");
//       for (uint8_t i = 0; i < responseLength; i++) {
//         Serial.print((char)response[i]); // Print the custom identifier string
//       }
//       Serial.println();
//     } else {
//       Serial.println("Handshake failed or app missing.");
//     }
    
//     delay(2000); // Wait before scanning again
//   }
// }

// void loop(void) {
//   // Kod AID dla usługi uwierzytelniania w Androidzie (np. FIDO/HCE)
//   uint8_t fido_aid[] = { 0xA0, 0x00, 0x00, 0x06, 0x47, 0x2F, 0x00, 0x01 }; 
//   uint8_t odpowiedz[255];
//   uint8_t dlugosc_odpowiedzi;

//   // Wysyłamy żądanie wyboru aplikacji o konkretnym AID do telefonu
//   bool sukces = nfc.inDataExchange(fido_aid, sizeof(fido_aid), odpowiedz, &dlugosc_odpowiedzi);

//   if (sukces) {
//     // Telefon odpowiedział na zapytanie o stałą usługę!
//     // Tablica 'odpowiedz' zawiera teraz STAŁY ciąg danych, unikalny dla Twojego Samsunga.
//     // Możesz go użyć w sekcji autoryzacji zamiast zmiennego UID.
//     Serial.print("Stały identyfikator telefonu: ");
//     for (uint8_t i = 0; i < dlugosc_odpowiedzi; i++) {
//       Serial.print(odpowiedz[i], HEX);
//     }
//     Serial.println("");
//     delay(2000);
//   }
//   else
//   {
//     Serial.println("Problem z komunikacją");
//     delay(2000);
//   }
// }

// void loop(void) { 
//   uint8_t success;
//   uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };  // Bufor na odczytany UID (do 7 bajtów)
//   uint8_t uidLength;                        // Długość odczytanego UID (4 lub 7 bajtów)
    
//   // Szukaj karty/telefonu w polu czytnika
//   success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength);
  
//   if (success) {
//     // 1. WYŚWIETLENIE KODU W KONSOLI (Użyj tego do skopiowania kodu swojego telefonu)
//     Serial.println("Wykryto urządzenie NFC!");
//     Serial.print("Długość UID: "); Serial.print(uidLength, DEC); Serial.println(" bajtów");
//     Serial.print("Kod UID w formacie HEX: ");
//     for (uint8_t i=0; i < uidLength; i++) {
//       Serial.print(" 0x");
//       if (uid[i] < 0x10) Serial.print("0");
//       Serial.print(uid[i], HEX);
//     }
//     Serial.println("");

//     // 2. WERYFIKACJA CZY ODRAZU PASUJE DO ZAUFANEGO KLUCZA
//     bool autoryzacjaOK = false;
    
//     if (uidLength == dlugosc_zaufanego_UID) {
//       autoryzacjaOK = true;
//       for (uint8_t i = 0; i < uidLength; i++) {
//         if (uid[i] != zaufany_UID[i]) {
//           autoryzacjaOK = false;
//           break;
//         }
//       }
//     }

//     // 3. REAKCJA SYSTEMU
//     if (autoryzacjaOK) {
//       Serial.println(">> DOSTĘP PRZYZNANY. Otwieram furtkę... <<");
//       digitalWrite(realyPin, LOW); // Włączenie przekaźnika (zwarcie styków elektrozaczepu)
//       delay(openingTime);
//       digitalWrite(realyPin, HIGH); // Wyłączenie przekaźnika
//       Serial.println("Furtka zamknięta.\n");
//     } else {
//       Serial.println(">> DOSTĘP ODGARDZONY. Nieznany klucz. <<\n");
//     }
    
//     // Krótka przerwa, aby uniknąć ciągłego odczytywania tego samego telefonu sekunda po sekundzie
//     delay(2000);
//   }
// }
