/*--------------------------------------------------------------------
this file is for the Esp32 camera 
which is an ESP32-S with an SD card module which we will be using
for testing
--------------------------------------------------------------------*/
/*
/*
Master code is based on an ESP32-S3 chip*/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include "Config.h"

//Puts Esp32 into Modem sleep for redused power consumption
static void ModemSleep();
static void I2C_send();

void setup(){
	Serial.begin(BAUDRATE);
	Wire.begin(SCL_PIN,SDA_PIN,slaveAdr); //Standard Frequency
	ModemSleep();
	pinMode(BILED,OUTPUT);
	Wire.onRequest(I2C_send);
}

void loop() {
	
}


static void ModemSleep(){
    esp_sleep_disable_wakeup_source;
  	btStop();
  	WiFi.mode(WIFI_OFF);
}

static void I2C_send() {
    digitalWrite(BILED, HIGH);
    Wire.write("Hello Wire"); // Send response
    digitalWrite(BILED, LOW);
}
