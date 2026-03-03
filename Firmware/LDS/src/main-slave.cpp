/*
Master code is based on an ESP32-S3 chip*/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include "Config.h"

//Puts Esp32 into Modem sleep for redused power consumption
static void ModemSleep();
String I2C_read(int numBytes);

void setup(){
	Serial.begin(BAUDRATE);
	Wire.begin(11,12); //Standard Frequency
	ModemSleep();
	pinMode(BILED,OUTPUT);
}

void loop() {
	Wire.requestFrom(slaveAdr, strlen("Hello Wire"));
	String msg = I2C_read(strlen("Hello Wire"));

    // Only print if a message was received
    if (msg.length() > 0) {
        Serial.println(msg);

        // Optional: blink LED as feedback
        digitalWrite(BILED, HIGH);
        delay(50);
        digitalWrite(BILED, LOW);
    }
	delay(500);
}

//=======================================================================


// Separate function for reading I2C data
String I2C_read(int numBytes) {
    String msg = "";
    int count = 0;

    while (Wire.available() && count < numBytes) {
        char c = Wire.read();
        msg += c;
        count++;
    }

    return msg;
}