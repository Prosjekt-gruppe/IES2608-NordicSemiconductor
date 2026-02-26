#include <Arduino.h>
#include <SD.h>
#include <Wire.h>
#include <WiFi.h>

//Puts Esp32 into Modem sleep for redused power consumption
static void ModemSleep(){
  esp_sleep_disable_wakeup_source;
  btStop();
  WiFi.mode(WIFI_OFF)
}

void setup(){
  // put your setup code here, to run once:

}

void loop() {
  // put your main code here, to run repeatedly:
}
