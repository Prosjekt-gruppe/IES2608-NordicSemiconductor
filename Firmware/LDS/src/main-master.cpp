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
	Wire.begin(SCL_PIN,SDA_PIN,masterAdr); //Standard Frequency
	ModemSleep();
	pinMode(BILED,OUTPUT);
}

void loop() {
	Serial.println(Wire.requestFrom(slaveAdr,sizeof("Hello Wire")));
	delay(500);
}


static void ModemSleep(){
    esp_sleep_disable_wakeup_source;
  	btStop();
  	WiFi.mode(WIFI_OFF);
}

