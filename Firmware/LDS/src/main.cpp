#include "Config.h"
#include "sdcard.h"

const char *trackingfile = "/Tracking.csv";  // CSV file
const char *PPKfile = "/PPK.csv";  // CSV file

uint32_t ppkcounter = 0; //file line indicator
uint32_t trackingcounter = 0;




//I2c data variables

typedef struct {
  float temperature;
  float humidity;
  float pressure;
  float ax;
  float ay;
  float az;
  float lat;
  float lon;
} sensor_data_t;

typedef struct {
  uint32_t timestamp;
  float current;
  float voltage;
} ppk_data_t;

#define BUFFER_SIZE 5
sensor_data_t lastSamples[BUFFER_SIZE];
volatile int idx = 0;

QueueHandle_t queueI2c;
QueueHandle_t queuePPK;

void receiveEvent(int bytes);
void requestEvent(); //called when thingy91x calls I2c_read
void SDTask(void *pvParameters);


//==============================================
void setup() {
  
  Serial.begin(BAUDRATE);
  ModemSleep();
  SPI.begin(SCLK, MISO, MOSI, CS);
  Wire.begin(SDA_PIN,SCL_PIN);
  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);
  SDinit();
  createFile(SD,trackingfile,"time,temperature,humidity,pressure,ax,ay,az,lat,lon");
  createFile(SD,PPKfile,"data num, current, e");
  //Parralell prosessing setup
  queueI2c = xQueueCreate(20, sizeof(sensor_data_t));
  queuePPK = xQueueCreate(20, sizeof(ppk_data_t));
  //xTaskCreatePinnedToCore(CDCTask, "CDC Task", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(SDTask, "SD Task", 8192, NULL, 1, NULL, 1);
}

void loop() {
  vTaskDelay(portMAX_DELAY);//safetydelay
  
}


//On Thingy Write
void receiveEvent(int bytes) {
  if (bytes != sizeof(sensor_data_t)) {
    while (Wire.available()) Wire.read();
    return;
  }

  sensor_data_t data;
  uint8_t *ptr = (uint8_t *)&data;

  for (int i = 0; i < sizeof(sensor_data_t); i++) {
    if (Wire.available()) {
      ptr[i] = Wire.read();
    }
  }

  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  xQueueSendFromISR(queueI2c, &data, &xHigherPriorityTaskWoken);
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);

  lastSamples[idx] = data;
  idx = (idx + 1) % BUFFER_SIZE;
}


//Thingy Read event
void requestEvent() {
  int pos = (idx - 1 + BUFFER_SIZE) % BUFFER_SIZE;

  Wire.write((uint8_t*)&lastSamples[pos], sizeof(sensor_data_t));
}



void SDTask(void *pvParameters) {
  sensor_data_t s;
  ppk_data_t p;

  while (1) {

    if (xQueueReceive(queueI2c, &s, 0) == pdTRUE) {

      File f = SD.open(trackingfile, FILE_APPEND);

      if (f) {
        f.printf("%lu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.6f,%.6f\n",
                 millis(),
                 s.temperature,
                 s.humidity,
                 s.pressure,
                 s.ax,
                 s.ay,
                 s.az,
                 s.lat,
                 s.lon);

        f.close();
      }
    }

    vTaskDelay(5);
  }
}