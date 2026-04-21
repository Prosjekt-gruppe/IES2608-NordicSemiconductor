#include "Config.h"
#include "sdcard.h"

const char *trackingfile = "/Tracking.csv";  // CSV file
const char *PPKfile = "/PPK.csv";  // CSV file

uint32_t ppkcounter = 0; //file line indicator
uint32_t trackingcounter = 0;


void receiveEvent(int bytes);

//==============================================
void setup() {
  
  Serial.begin(BAUDRATE);
  ModemSleep();
  SPI.begin(SCLK, MISO, MOSI, CS);
  Wire.begin(SDA_PIN,SCL_PIN);
  Wire.onReceive(receiveEvent);
  SDinit();
  createFile(SD,trackingfile,"data num, Lat, Lon, Accuracy");
  createFile(SD,PPKfile,"data num, current, e");
}

void loop() {
  
  // Open file in append mode

  delay(5000); // wait 5 seconds before next write
  
}


void receiveEvent(int bytes){
  while (Wire.available()){
    String data = data + (String)Wire.read();
  }
}