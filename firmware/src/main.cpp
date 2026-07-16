#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <driver/i2s.h>
#include <Wire.h>

// --- Network Settings ---
const char* ssid = "fixbalau_2.4GHz";
const char* password = "FX@BPAU";
const char* server_ip = "192.168.1.9"; // Target machine running the FastAPI backend
const uint16_t server_port = 8000;

WebSocketsClient webSocket;

// --- ESP32-S3-BOX-3 Official Hardware Mappings ---
#define I2C_SDA         GPIO_NUM_41
#define I2C_SCL         GPIO_NUM_40
#define PA_ENABLE_PIN   GPIO_NUM_46

// We MUST use a single port since the Speaker and Mic share the exact same physical clock pins
#define I2S_PORT        I2S_NUM_0

#define I2S_MCLK        GPIO_NUM_2
#define I2S_BCLK        GPIO_NUM_17  
#define I2S_WS          GPIO_NUM_45  
#define I2S_DOUT        GPIO_NUM_15  
#define I2S_DIN         GPIO_NUM_16  

#define ES8311_ADDR     0x18
#define ES7210_ADDR     0x40

void i2c_write_reg(uint8_t chip_addr, uint8_t reg_addr, uint8_t value) {
  Wire.beginTransmission(chip_addr);
  Wire.write(reg_addr);
  Wire.write(value);
  Wire.endTransmission();
  delay(5);
}

// ---------------------------------------------------------
// 1. Unified Audio Pipeline (24 kHz for both RX & TX)
// ---------------------------------------------------------
void setup_audio_pipeline() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
    .sample_rate = 24000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // Mono
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 512,
    .use_apll = false
  };
  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  
  i2s_pin_config_t pin_config = {
    .mck_io_num = I2S_MCLK, // CRITICAL FIX: Injects the Master Clock
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_DOUT,
    .data_in_num = I2S_DIN
  };
  i2s_set_pin(I2S_PORT, &pin_config);
  
  Serial.println("[HARDWARE] Unified Audio I2S Pipeline Initialized.");
}

// ---------------------------------------------------------
// 2. Audio Codec Control
// ---------------------------------------------------------
void init_audio_codecs() {
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  pinMode(PA_ENABLE_PIN, OUTPUT);
  digitalWrite(PA_ENABLE_PIN, HIGH); 
  delay(100); 

  Serial.println("[HARDWARE] Configuring ES8311 (Speaker)...");
  i2c_write_reg(ES8311_ADDR, 0x00, 0x80); 
  i2c_write_reg(ES8311_ADDR, 0x00, 0x00); 
  i2c_write_reg(ES8311_ADDR, 0x01, 0x30); 
  i2c_write_reg(ES8311_ADDR, 0x0D, 0x02); 
  i2c_write_reg(ES8311_ADDR, 0x0E, 0x00); 
  i2c_write_reg(ES8311_ADDR, 0x14, 0x90); 

  Serial.println("[HARDWARE] Configuring ES7210 (Mic)...");
  i2c_write_reg(ES7210_ADDR, 0x00, 0xFF); 
  i2c_write_reg(ES7210_ADDR, 0x01, 0x1C); 
  i2c_write_reg(ES7210_ADDR, 0x0E, 0x03); 
  i2c_write_reg(ES7210_ADDR, 0x13, 0x10); 
}

// ---------------------------------------------------------
// 3. WebSocket Processing
// ---------------------------------------------------------
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  if (type == WStype_BIN) {
    size_t bytes_written;
    i2s_write(I2S_PORT, payload, length, &bytes_written, portMAX_DELAY);
  } 
  else if (type == WStype_CONNECTED) {
    Serial.println("[NET] Connected to FastAPI Gateway.");
  }
}

// ---------------------------------------------------------
// 4. Main Routines
// ---------------------------------------------------------
void setup() {
  Serial.begin(115200);
  
  WiFi.begin(ssid, password);
  while(WiFi.status() != WL_CONNECTED) { 
    delay(500); 
    Serial.print("."); 
  }
  Serial.println("\n[NET] WiFi Connected successfully.");

  // CRITICAL FIX: Start the I2S clocks FIRST, so the chips have a heartbeat when I2C config arrives
  setup_audio_pipeline();
  init_audio_codecs();

  webSocket.begin(server_ip, server_port, "/ws/voice"); 
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
}

void loop() {
  webSocket.loop();
  
  uint8_t mic_buffer[512]; 
  size_t bytes_read = 0;
  
  // Read from the unified port
  i2s_read(I2S_PORT, mic_buffer, sizeof(mic_buffer), &bytes_read, portMAX_DELAY);
  
  if (bytes_read > 0 && webSocket.isConnected()) {
    webSocket.sendBIN(mic_buffer, bytes_read);
  }
}
