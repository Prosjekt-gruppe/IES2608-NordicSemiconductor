/*
This file is for sdcard read/write development and testing
*/
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include "Config.h"

// MicroSD Libraries
#include "FS.h"
#include <SD.h>
#include <SPI.h>
// EEPROM Library
#include "EEPROM.h"
 
// Use 1 byte of EEPROM space
#define EEPROM_SIZE 1



void initMicroSDCard();
void testfile();
File initLogFile(const char* filename);

File datafile;

void setup(){
    Serial.begin(BAUDRATE);
	SD.begin(4);
    //pinMode(4,OUTPUT);
	//pinMode(0,INPUT);
	ModemSleep();
	//testfile();
    datafile = initLogFile("/ESP32_nano.csv");
    Serial.println("File initialized");
    
}
	
void loop() {
    datafile.print(12000);
    datafile.println(",Data here");
    Serial.println("Data written to sd card");
	delay(10000);
}


//Initilases file and writes CSV header if none are present
File initLogFile(const char* filename) {
    File file = SD.open(filename, FILE_WRITE);

    if (!file) {
        Serial.println("Failed to open log file");
        return file;
    }

    if (file.size() == 0) {
        file.println("time_ms,data");
        file.flush();
    }

    return file;
}
