/*
This file is for sdcard read/write development and testing
*/
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include "Config.h"

// MicroSD Libraries
#include "FS.h"
#include "SD_MMC.h"
#include "sd.h"
// EEPROM Library
#include "EEPROM.h"
 
// Use 1 byte of EEPROM space
#define EEPROM_SIZE 1



void initMicroSDCard();
void testfile();

void setup(){
	Serial.begin(BAUDRATE);
	pinMode(4,OUTPUT);
	pinMode(0,INPUT);
	ModemSleep();
	
	testfile();
}
	
void loop() {
	delay(10000);
}


void saveCSVHeader() {
    // Write column headers to CSV
    const char* header = "Time,position,current use\n"; // Example columns
    writeFile(SD_MMC, "/data.csv", header);
}
void appendCSVData(float temperature, float humidity) {
    char buffer[64];
    
    // Format the data as CSV: "time,temperature,humidity"
    // Using millis() for time example
    sprintf(buffer, "%lu,%.2f,%.2f\n", millis()/1000, temperature, humidity);
    
    appendFile(SD_MMC, "/data.csv", buffer);
}

void testfile(){
	if(!SD_MMC.begin()){
        Serial.println("Card Mount Failed");
        return;
    }
    uint8_t cardType = SD_MMC.cardType();

    if(cardType == CARD_NONE){
        Serial.println("No SD_MMC card attached");
        return;
    }

    Serial.print("SD_MMC Card Type: ");
    if(cardType == CARD_MMC){
        Serial.println("MMC");
    } else if(cardType == CARD_SD){
        Serial.println("SDSC");
    } else if(cardType == CARD_SDHC){
        Serial.println("SDHC");
    } else {
        Serial.println("UNKNOWN");
    }

    uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
    Serial.printf("SD_MMC Card Size: %lluMB\n", cardSize);

    writeFile(SD_MMC, "/hello.csv", "Hello ");
    appendFile(SD_MMC, "/hello.csv", "SD card!\n");
    readFile(SD_MMC, "/hello.csv");
    saveCSVHeader();
	appendCSVData(10000, 696969);
	appendCSVData(10001, 416969);
	for (int i = 0; i < 5000; i++){
		appendCSVData(random(10546842), random(1000));
	}
}