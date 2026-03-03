

//I2C config
#define SDA_PIN 21
#define SCL_PIN 22
#define CAM_SDA_PIN 14
#define CAM_SCL_PIN 15
#define masterAdr 0x00
#define slaveAdr 0x01

//Serial config
#define BAUDRATE 115200


//GPIO definitions
#define BILED 2 //Buildt in led


static void ModemSleep(){
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  	btStop();
  	WiFi.mode(WIFI_OFF);
}