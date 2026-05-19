#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>

// SD-card stack used by the ESP32 logger.
#include <SD.h>
#include <SPI.h>
#include <FS.h>

// Arduino Nano ESP32 pin mapping for I2C and the SD-card SPI bus.
#define SDA_PIN A4
#define SCL_PIN A5
#define masterAdr 0x00
#define slaveAdr 0x0f
#define CS D4         
#define SCLK D13
#define MISO D12
#define MOSI D11

#define BAUDRATE 115200

static void ModemSleep(){
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  	btStop();
  	WiFi.mode(WIFI_OFF);
}
