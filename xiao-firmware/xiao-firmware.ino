#define CAMERA_MODEL_XIAO_ESP32S3
#include <I2S.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include "esp_camera.h"
#include "camera_pins.h"
#include "mulaw.h"

//
// BLE
//

static BLEUUID serviceUUID("19B10000-E8F2-537E-4F6C-D104768A1214");
static BLEUUID audioCharUUID("19B10001-E8F2-537E-4F6C-D104768A1214");
static BLEUUID audioCodecUUID("19B10002-E8F2-537E-4F6C-D104768A1214");
static BLEUUID photoCharUUID("19B10005-E8F2-537E-4F6C-D104768A1214");
static BLEUUID commandUUID("19B10006-E8F2-537E-4F6C-D104768A1214");

BLECharacteristic *audio;
BLECharacteristic *photo;
BLECharacteristic *command;
bool connected = false;

class ServerHandler: public BLEServerCallbacks
{
  void onConnect(BLEServer *server)
  {
    connected = true;
    Serial.println("Connected");
  }

  void onDisconnect(BLEServer *server)
  {
    connected = false;
    Serial.println("Disconnected");
    BLEDevice::startAdvertising();
  }
};

void configure_ble() {
  BLEDevice::init("OpenGlass");
  BLEServer *server = BLEDevice::createServer();
  BLEService *service = server->createService(serviceUUID);

  // Audio service
  audio = service->createCharacteristic(
    audioCharUUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  BLE2902 *ccc = new BLE2902();
  ccc->setNotifications(true);
  audio->addDescriptor(ccc);

  // Photo service
  photo = service->createCharacteristic(
    photoCharUUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  ccc = new BLE2902();
  ccc->setNotifications(true);
  photo->addDescriptor(ccc);

  // Codec service
  BLECharacteristic *codec = service->createCharacteristic(
    audioCodecUUID,
    BLECharacteristic::PROPERTY_READ
  );
  uint8_t codecId = 11; // MuLaw 8mhz
  codec->setValue(&codecId, 1);

  //TTS service
  command = service->createCharacteristic(
    commandUUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  ccc = new BLE2902();
  ccc->setNotifications(true);
  command->addDescriptor(ccc);

  // Service
  server->setCallbacks(new ServerHandler());
  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(service->getUUID());
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x0);
  advertising->setMinPreferred(0x1F);
  BLEDevice::startAdvertising();
}

camera_fb_t *fb;

bool take_photo() {

  // Release buffer
  if (fb) {
    Serial.println("Release FB");
    esp_camera_fb_return(fb);
  }

  // Take a photo
  Serial.println("Taking photo...");
  fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Failed to get camera frame buffer");
    return false;
  }

  return true;
}

//
// Microphone
//

#define VOLUME_GAIN 3

static size_t recording_buffer_size = 200;
static size_t compressed_buffer_size = 493 + 2; /* header */
static uint8_t *s_recording_buffer = nullptr;
static uint8_t *s_compressed_frame = nullptr;
static uint8_t *s_compressed_frame_2 = nullptr;

void configure_microphone() {

  // start I2S at 16 kHz with 16-bits per sample
  I2S.setAllPins(-1, 42, 41, -1, -1);
  if (!I2S.begin(PDM_MONO_MODE, 16000, 16)) {
    Serial.println("Failed to initialize I2S!");
    while (1); // do nothing
  }

  // Allocate buffers
  s_recording_buffer = (uint8_t *) ps_calloc(recording_buffer_size, sizeof(uint8_t));
  s_compressed_frame = (uint8_t *) ps_calloc(compressed_buffer_size, sizeof(uint8_t));
  s_compressed_frame_2 = (uint8_t *) ps_calloc(compressed_buffer_size, sizeof(uint8_t));
}

size_t read_microphone() {
  size_t bytes_recorded = 0;
  esp_i2s::i2s_read(esp_i2s::I2S_NUM_0, s_recording_buffer, recording_buffer_size, &bytes_recorded, portMAX_DELAY);
  return bytes_recorded;
}

//
// Camera
//

void configure_camera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG; // for streaming
  config.fb_count = 1;

  config.jpeg_quality = 10;
  config.frame_size = FRAMESIZE_QVGA;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }
}

//
// Main
//


void setup() {
  Serial.begin(921600);
  Serial.println("Setup");
  Serial.println("Starting BLE...");
  configure_ble();
  Serial.println("Starting Microphone...");
  configure_microphone();
  Serial.println("Starting Camera...");
  configure_camera();
  Serial.println("OK");
}

uint16_t frame_count = 0;
unsigned long lastCaptureTime = 0;
size_t sent_photo_bytes = 0;
size_t sent_photo_frames = 0;
bool need_send_photo = false;

bool toggled = false;
bool touching = false;

void loop() {
  //handle touch buttons:
  uint32_t touch0 = touchRead(D0);
  if((touch0 > 30000) && !touching) {
    touching = true;
    toggled = !toggled;
    uint8_t data = toggled ? 255 : 0;
    command->setValue(&data, 1);
    command->notify();
    Serial.println(data);
  } else if(touch0 <= 30000) {
    touching = false;
  }

  // Read from mic
  size_t bytes_recorded = read_microphone();

  // Push to BLE
  if (bytes_recorded > 0 && connected && toggled) {

    for (size_t i = 0; i < bytes_recorded; i += 2) {
      (*(uint16_t *)(s_recording_buffer+i)) <<= VOLUME_GAIN;
    }
    audio->setValue(s_recording_buffer, bytes_recorded);
    audio->notify();
    frame_count++;
  }

  // Take a photo
  unsigned long now = millis();
  if ((now - lastCaptureTime) >= 5000 && !need_send_photo && connected && !toggled) {
    if (take_photo()) {
      need_send_photo = true;
      sent_photo_bytes = 0;
      sent_photo_frames = 0;
      lastCaptureTime = now;
    }
  }

  // Push to BLE
  if (need_send_photo && !toggled) {
    size_t remaining = fb->len - sent_photo_bytes;
    Serial.println("packet: " + String(sent_photo_frames));
    if (remaining > 0) {
      // Populate buffer
      s_compressed_frame_2[0] = sent_photo_frames & 0xFF;         //
      s_compressed_frame_2[1] = (sent_photo_frames >> 8) & 0xFF;  //packet number
      size_t bytes_to_copy = remaining;
      if (bytes_to_copy > 493) {  
        bytes_to_copy = 493;
      }
      memcpy(&s_compressed_frame_2[2], &fb->buf[sent_photo_bytes], bytes_to_copy);  //all the rest of the photo data
      
      // Push to BLE
      photo->setValue(s_compressed_frame_2, bytes_to_copy + 2);
      photo->notify();
      sent_photo_bytes += bytes_to_copy;
      sent_photo_frames++;
    } else {

      // End flag
      s_compressed_frame_2[0] = 0xFF;
      s_compressed_frame_2[1] = 0xFF;
      photo->setValue(s_compressed_frame_2, 2);
      photo->notify();

      Serial.println("Photo sent");
      need_send_photo = false;
    }
  }

  // Delay to ensure proper transmission of bluetooth packets
  delay(4);
}
