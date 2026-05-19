#include "Config.h"
#include "sdcard.h"

// CSV files written by the ESP32 logger.
const char *trackingfile = "/Tracking.csv";
const char *PPKfile = "/PPK.csv";

uint32_t ppkcounter = 0;
uint32_t trackingcounter = 0;




// Payload format expected from the Thingy. Both sides must keep this struct in sync.
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
void requestEvent();
void SDTask(void *pvParameters);

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

  // The I2C callback runs like an interrupt, so it only queues data for the SD task.
  queueI2c = xQueueCreate(20, sizeof(sensor_data_t));
  queuePPK = xQueueCreate(20, sizeof(ppk_data_t));
  xTaskCreatePinnedToCore(SDTask, "SD Task", 8192, NULL, 1, NULL, 1);
}

void loop() {
  vTaskDelay(portMAX_DELAY);
  
}

// Called when the Thingy writes one sensor_data_t over I2C.
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

// Called when the Thingy reads back the latest stored sample.
void requestEvent() {
  int pos = (idx - 1 + BUFFER_SIZE) % BUFFER_SIZE;

  Wire.write((uint8_t*)&lastSamples[pos], sizeof(sensor_data_t));
}

// SD writes happen in a normal FreeRTOS task, not inside the I2C callback.
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
