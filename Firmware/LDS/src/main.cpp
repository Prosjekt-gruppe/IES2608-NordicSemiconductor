#include "Config.h"
#include "sdcard.h"

const char *trackingfile = "/Tracking.csv";  // CSV file
const char *PPKfile = "/PPK.csv";  // CSV file

uint32_t ppkcounter = 0; //file line indicator
uint32_t trackingcounter = 0;

void setup() {
  
  Serial.begin(BAUDRATE);
  ModemSleep();
  SPI.begin(SCLK, MISO, MOSI, CS);

  // Initialize SD card at 25 MHz (safe for most SD modules)
  if (!SD.begin(CS, SPI, 25000000)) {
    Serial.println("Card Mount Failed");
    while (true) { delay(1000); } // halt safely
  }
  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("No SD card attached");
    while (true) { delay(1000); } // halt safely
  }
  Serial.println("SD card initialized successfully!");

  createFile(SD,trackingfile,"data num, Lat, Lon, Accuracy");
  createFile(SD,PPKfile,"data num, current, e");
}

void loop() {
  
  // Open file in append mode

  delay(5000); // wait 5 seconds before next write
  
}