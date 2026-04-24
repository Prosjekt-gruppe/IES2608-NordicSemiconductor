#include "Config.h"
#include "sdcard.h"

#include <stddef.h>
#include <string.h>

const char *trackingfile = "/Tracking.csv";
const char *PPKfile = "/PPK.csv";
const char *fieldlogfile = "/fieldlog.bin";

uint32_t ppkcounter = 0;
uint32_t trackingcounter = 0;

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

#define LDS_FRAME_MAGIC 0x3153444cUL
#define LDS_STATUS_MAGIC 0x3153544cUL
#define LDS_PROTOCOL_VERSION 1
#define LDS_CMD_FIELD_LOG_APPEND 1
#define LDS_FIELD_LOG_RECORD_SIZE 32
#define LDS_FIELD_LOG_RECORDS_PER_FRAME 2
#define LDS_FIELD_LOG_PAYLOAD_BYTES (LDS_FIELD_LOG_RECORD_SIZE * LDS_FIELD_LOG_RECORDS_PER_FRAME)
#define LDS_NO_SEQUENCE 0xffffffffUL

#define LDS_STATUS_OK 0
#define LDS_STATUS_PENDING 1
#define LDS_STATUS_BUSY 2
#define LDS_STATUS_BAD_FRAME 3
#define LDS_STATUS_SD_ERROR 4
#define LDS_STATUS_QUEUE_FULL 5

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint8_t version;
  uint8_t command;
  uint16_t frame_len;
  uint32_t first_sequence;
  uint8_t record_count;
  uint8_t record_size;
  uint16_t payload_len;
  uint16_t payload_crc16;
  uint8_t payload[LDS_FIELD_LOG_PAYLOAD_BYTES];
  uint16_t frame_crc16;
} lds_field_log_frame_t;

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint8_t version;
  uint8_t status;
  uint16_t reserved;
  uint32_t committed_frames;
  uint32_t committed_records;
  uint32_t last_committed_sequence;
  uint32_t rejected_frames;
  uint16_t crc16;
} lds_status_response_t;

typedef struct {
  uint32_t first_sequence;
  uint8_t record_count;
  uint16_t payload_len;
  uint8_t payload[LDS_FIELD_LOG_PAYLOAD_BYTES];
} lds_field_log_chunk_t;

static_assert(sizeof(lds_field_log_frame_t) <= 128, "LDS frame must fit common I2C slave buffers");

#define BUFFER_SIZE 5
sensor_data_t lastSamples[BUFFER_SIZE];
volatile int idx = 0;

QueueHandle_t queueI2c;
QueueHandle_t queuePPK;
QueueHandle_t queueFieldLog;

static lds_status_response_t ldsStatus;

void receiveEvent(int bytes);
void requestEvent();
void SDTask(void *pvParameters);

static uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
  uint16_t crc = 0xffff;

  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;

    for (int bit = 0; bit < 8; bit++) {
      if ((crc & 0x8000) != 0) {
        crc = (crc << 1) ^ 0x1021;
      } else {
        crc <<= 1;
      }
    }
  }

  return crc;
}

static void updateStatusCrc() {
  ldsStatus.crc16 = crc16_ccitt((const uint8_t *)&ldsStatus,
                                offsetof(lds_status_response_t, crc16));
}

static void setStatus(uint8_t status) {
  ldsStatus.status = status;
  updateStatusCrc();
}

static void initStatus() {
  memset(&ldsStatus, 0, sizeof(ldsStatus));
  ldsStatus.magic = LDS_STATUS_MAGIC;
  ldsStatus.version = LDS_PROTOCOL_VERSION;
  ldsStatus.status = LDS_STATUS_OK;
  ldsStatus.last_committed_sequence = LDS_NO_SEQUENCE;
  updateStatusCrc();
}

static bool ensureBinaryFile(fs::FS &fs, const char *path) {
  if (fs.exists(path)) {
    return true;
  }

  File file = fs.open(path, FILE_WRITE);
  if (!file) {
    return false;
  }

  file.close();
  return true;
}

static bool decodeFieldLogRecordSequence(const uint8_t record[LDS_FIELD_LOG_RECORD_SIZE],
                                         uint32_t *sequence) {
  uint16_t magic = (uint16_t)record[0] | ((uint16_t)record[1] << 8);
  uint8_t version = record[2];
  uint8_t recordType = record[3];
  uint16_t storedCrc = (uint16_t)record[30] | ((uint16_t)record[31] << 8);
  uint16_t expectedCrc = crc16_ccitt(record, LDS_FIELD_LOG_RECORD_SIZE - 2);

  if (magic != 0x464c ||
      version != 2 ||
      (recordType != 1 && recordType != 2) ||
      storedCrc != expectedCrc) {
    return false;
  }

  *sequence = (uint32_t)record[4] |
              ((uint32_t)record[5] << 8) |
              ((uint32_t)record[6] << 16) |
              ((uint32_t)record[7] << 24);
  return true;
}

static void loadFieldLogStatusFromFile() {
  uint8_t record[LDS_FIELD_LOG_RECORD_SIZE];
  uint32_t sequence;

  if (!SD.exists(fieldlogfile)) {
    return;
  }

  File file = SD.open(fieldlogfile, FILE_READ);
  if (!file) {
    setStatus(LDS_STATUS_SD_ERROR);
    return;
  }

  size_t usableSize = file.size() - (file.size() % LDS_FIELD_LOG_RECORD_SIZE);

  for (int32_t offset = (int32_t)usableSize - LDS_FIELD_LOG_RECORD_SIZE;
       offset >= 0;
       offset -= LDS_FIELD_LOG_RECORD_SIZE) {
    if (!file.seek(offset)) {
      continue;
    }

    if (file.read(record, sizeof(record)) != sizeof(record)) {
      continue;
    }

    if (decodeFieldLogRecordSequence(record, &sequence)) {
      ldsStatus.committed_records = usableSize / LDS_FIELD_LOG_RECORD_SIZE;
      ldsStatus.last_committed_sequence = sequence;
      setStatus(LDS_STATUS_OK);
      break;
    }
  }

  file.close();
}

static bool validateFieldLogFrame(const lds_field_log_frame_t *frame) {
  uint16_t expectedPayloadCrc;
  uint16_t expectedFrameCrc;

  if (frame->magic != LDS_FRAME_MAGIC ||
      frame->version != LDS_PROTOCOL_VERSION ||
      frame->command != LDS_CMD_FIELD_LOG_APPEND ||
      frame->frame_len != sizeof(lds_field_log_frame_t)) {
    return false;
  }

  if (frame->record_count == 0 ||
      frame->record_count > LDS_FIELD_LOG_RECORDS_PER_FRAME ||
      frame->record_size != LDS_FIELD_LOG_RECORD_SIZE ||
      frame->payload_len != frame->record_count * LDS_FIELD_LOG_RECORD_SIZE) {
    return false;
  }

  expectedPayloadCrc = crc16_ccitt(frame->payload, frame->payload_len);
  if (frame->payload_crc16 != expectedPayloadCrc) {
    return false;
  }

  expectedFrameCrc = crc16_ccitt((const uint8_t *)frame,
                                 offsetof(lds_field_log_frame_t, frame_crc16));
  return frame->frame_crc16 == expectedFrameCrc;
}

static void queueSensorSample(int bytes) {
  sensor_data_t data;
  uint8_t *ptr = (uint8_t *)&data;

  if (bytes != sizeof(sensor_data_t)) {
    while (Wire.available()) {
      Wire.read();
    }
    return;
  }

  memset(&data, 0, sizeof(data));
  for (int i = 0; i < sizeof(sensor_data_t); i++) {
    if (Wire.available()) {
      ptr[i] = Wire.read();
    }
  }

  BaseType_t higherPriorityTaskWoken = pdFALSE;
  xQueueSendFromISR(queueI2c, &data, &higherPriorityTaskWoken);
  portYIELD_FROM_ISR(higherPriorityTaskWoken);

  lastSamples[idx] = data;
  idx = (idx + 1) % BUFFER_SIZE;
}

static void queueFieldLogFrame(int bytes) {
  lds_field_log_frame_t frame;
  lds_field_log_chunk_t chunk;
  uint8_t *ptr = (uint8_t *)&frame;

  if (bytes != sizeof(lds_field_log_frame_t)) {
    while (Wire.available()) {
      Wire.read();
    }
    ldsStatus.rejected_frames++;
    Serial.printf("LDS RX invalid length: got=%d expected=%u\n",
                  bytes,
                  (unsigned)sizeof(lds_field_log_frame_t));
    setStatus(LDS_STATUS_BAD_FRAME);
    return;
  }

  memset(&frame, 0, sizeof(frame));
  for (int i = 0; i < sizeof(lds_field_log_frame_t); i++) {
    if (Wire.available()) {
      ptr[i] = Wire.read();
    }
  }

  if (!validateFieldLogFrame(&frame)) {
    ldsStatus.rejected_frames++;
    Serial.printf("LDS RX bad frame: seq=%lu records=%u payload=%u\n",
                  (unsigned long)frame.first_sequence,
                  frame.record_count,
                  frame.payload_len);
    setStatus(LDS_STATUS_BAD_FRAME);
    return;
  }

  memset(&chunk, 0, sizeof(chunk));
  chunk.first_sequence = frame.first_sequence;
  chunk.record_count = frame.record_count;
  chunk.payload_len = frame.payload_len;
  memcpy(chunk.payload, frame.payload, frame.payload_len);

  BaseType_t higherPriorityTaskWoken = pdFALSE;
  if (xQueueSendFromISR(queueFieldLog, &chunk, &higherPriorityTaskWoken) != pdTRUE) {
    ldsStatus.rejected_frames++;
    Serial.printf("LDS RX queue full: seq=%lu records=%u payload=%u\n",
                  (unsigned long)chunk.first_sequence,
                  chunk.record_count,
                  chunk.payload_len);
    setStatus(LDS_STATUS_QUEUE_FULL);
    return;
  }

  Serial.printf("LDS RX fieldlog: seq=%lu..%lu records=%u payload=%u\n",
                (unsigned long)chunk.first_sequence,
                (unsigned long)(chunk.first_sequence + chunk.record_count - 1),
                chunk.record_count,
                chunk.payload_len);
  setStatus(LDS_STATUS_PENDING);
  portYIELD_FROM_ISR(higherPriorityTaskWoken);
}

void setup() {
  Serial.begin(BAUDRATE);
  ModemSleep();
  SPI.begin(SCLK, MISO, MOSI, CS);
  initStatus();
  SDinit();

  createFile(SD, trackingfile, "time,temperature,humidity,pressure,ax,ay,az,lat,lon");
  createFile(SD, PPKfile, "data num, current, e");
  if (!ensureBinaryFile(SD, fieldlogfile)) {
    Serial.println("Failed to create fieldlog.bin");
  }
  loadFieldLogStatusFromFile();

  queueI2c = xQueueCreate(20, sizeof(sensor_data_t));
  queuePPK = xQueueCreate(20, sizeof(ppk_data_t));
  queueFieldLog = xQueueCreate(20, sizeof(lds_field_log_chunk_t));

  updateStatusCrc();

  Wire.begin((uint8_t)slaveAdr, SDA_PIN, SCL_PIN);
  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);

  xTaskCreatePinnedToCore(SDTask, "SD Task", 8192, NULL, 1, NULL, 1);
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}

void receiveEvent(int bytes) {
  if (bytes == sizeof(lds_field_log_frame_t)) {
    queueFieldLogFrame(bytes);
    return;
  }

  queueSensorSample(bytes);
}

void requestEvent() {
  updateStatusCrc();
  Wire.write((uint8_t *)&ldsStatus, sizeof(ldsStatus));
}

void SDTask(void *pvParameters) {
  sensor_data_t s;
  lds_field_log_chunk_t chunk;

  while (1) {
    if (xQueueReceive(queueFieldLog, &chunk, 0) == pdTRUE) {
      File f = SD.open(fieldlogfile, FILE_APPEND);
      size_t written = 0;

      if (f) {
        written = f.write(chunk.payload, chunk.payload_len);
      }

      if (f && written == chunk.payload_len) {
        f.close();
        ldsStatus.committed_frames++;
        ldsStatus.committed_records += chunk.record_count;
        ldsStatus.last_committed_sequence = chunk.first_sequence + chunk.record_count - 1;
        Serial.printf("LDS SD stored: seq=%lu..%lu bytes=%u total_records=%lu\n",
                      (unsigned long)chunk.first_sequence,
                      (unsigned long)(chunk.first_sequence + chunk.record_count - 1),
                      chunk.payload_len,
                      (unsigned long)ldsStatus.committed_records);
        setStatus(LDS_STATUS_OK);
      } else {
        if (f) {
          f.close();
        }
        Serial.printf("LDS SD write failed: seq=%lu..%lu requested=%u written=%u\n",
                      (unsigned long)chunk.first_sequence,
                      (unsigned long)(chunk.first_sequence + chunk.record_count - 1),
                      chunk.payload_len,
                      (unsigned)written);
        setStatus(LDS_STATUS_SD_ERROR);
      }
    }

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
