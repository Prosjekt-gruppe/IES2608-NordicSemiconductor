#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include "Config.h"

//Puts Esp32 into Modem sleep for redused power consumption
static void ModemSleep();
static void I2C_send();

void setup(){
	Serial.begin(BAUDRATE);
	Wire.begin(SCL_PIN,SDA_PIN); //Standard Frequency
	ModemSleep();
}

void loop() {
	Wire.onRequest(I2C_send);
}


static void ModemSleep(){
    esp_sleep_disable_wakeup_source;
  	btStop();
  	WiFi.mode(WIFI_OFF);
}

static void I2C_send(byte data){
    Wire.beginTransmission(slaveAdr);
    Wire.write((data, sizeof(data)));
    Wire.endTransmission();
    Serial.println("Data sendt");
}