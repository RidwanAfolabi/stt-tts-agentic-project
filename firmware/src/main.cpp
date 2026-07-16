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
#define PA_ENABLE_PIN   GPIO_NUM_46  // Power Amplifier Control Pin

#define I2S_PORT_SPK    I2S_NUM_0
#define I2S_PORT_MIC    I2S_NUM_1

#define I2S_MCLK        GPIO_NUM_2
#define I2S_BCLK        GPIO_NUM_17  // Clock line (SCLK)
#define I2S_WS          GPIO_NUM_45  // Word Select line (LCLK)
#define I2S_DOUT        GPIO_NUM_15  // Data Out routing to the Speaker
#define I2S_DIN         GPIO_NUM_16  // Data In coming from the Microphones

// Chip I2C Addresses
#define ES8311_ADDR     0x18
#define ES7210_ADDR     0x40

// ---------------------------------------------------------
// Helper Function: Write Data to a Specific Chip Register
// ---------------------------------------------------------
void i2c_write_reg(uint8_t chip_addr, uint8_t reg_addr, uint8_t value) {
  Wire.beginTransmission(chip_addr);
  Wire.write(reg_addr);
  Wire.write(value);
  Wire.endTransmission();
  delay(5); // Small delay to allow the register to settle
}

// ---------------------------------------------------------
// 1. Audio Codec Control (Powering On & Tuning the Hardware)
// ---------------------------------------------------------
void init_audio_codecs() {
  // Open the master I2C protocol line at 400 kHz
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  Serial.println("[HARDWARE] Codec I2C Bus Initialized.");
  
  // Power up the physical speaker amplifier circuit
  pinMode(PA_ENABLE_PIN, OUTPUT);
  digitalWrite(PA_ENABLE_PIN, HIGH); 
  Serial.println("[HARDWARE] Onboard Speaker Amplifier Power Mainframe Enabled.");
  delay(100); // Allow voltage rails to completely stabilize

  // --- Waking up the ES8311 (Speaker DAC) ---
  Serial.println("[HARDWARE] Configuring ES8311 Registers...");
  i2c_write_reg(ES8311_ADDR, 0x00, 0x80); // Trigger master reset
  i2c_write_reg(ES8311_ADDR, 0x00, 0x00); // Clear reset state
  i2c_write_reg(ES8311_ADDR, 0x01, 0x30); // Enable internal system clocks
  i2c_write_reg(ES8311_ADDR, 0x0D, 0x02); // Power up the digital-to-analog converter (DAC)
  i2c_write_reg(ES8311_ADDR, 0x0E, 0x00); // Clear mute flag on the analog output line
  i2c_write_reg(ES8311_ADDR, 0x14, 0x90); // Set system playback volume to a comfortable level

  // --- Waking up the ES7210 (Dual Microphone ADC) ---
  Serial.println("[HARDWARE] Configuring ES7210 Registers...");
  i2c_write_reg(ES7210_ADDR, 0x00, 0xFF); // Reset all internal register spaces
  i2c_write_reg(ES7210_ADDR, 0x01, 0x1C); // Power up master analog reference systems
  i2c_write_reg(ES7210_ADDR, 0x0E, 0x03); // Enable Analog Channel 1 & Channel 2 (Dual MIC array)
  i2c_write_reg(ES7210_ADDR, 0x13, 0x10); // Set the microphone preamplifier gain levels
  
  Serial.println("[HARDWARE] Audio Hardware Codecs Fully Configured and Awake.");
}

// ---------------------------------------------------------
// 2. Audio Pipeline Drivers (16 kHz Mic, 24 kHz Speaker)
// ---------------------------------------------------------
void setup_audio_pipeline() {
  // Config for the Speaker (Receiving 24 kHz Mono from Gemini)
  i2s_config_t spk_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 24000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 512,
    .use_apll = false
  };
  i2s_driver_install(I2S_PORT_SPK, &spk_config, 0, NULL);
  
  i2s_pin_config_t spk_pins = {
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_set_pin(I2S_PORT_SPK, &spk_pins);

  // Config for the Microphone Array (Streaming 16 kHz Mono to Gemini)
  i2s_config_t mic_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 512,
    .use_apll = false
  };
  i2s_driver_install(I2S_PORT_MIC, &mic_config, 0, NULL);
  
  i2s_pin_config_t mic_pins = {
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_DIN
  };
  i2s_set_pin(I2S_PORT_MIC, &mic_pins);
  
  Serial.println("[HARDWARE] Audio I2S Pipelines Initialized.");
}

// ---------------------------------------------------------
// 3. WebSocket Processing
// ---------------------------------------------------------
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  if (type == WStype_BIN) {
    size_t bytes_written;
    // Inject the incoming binary audio buffer straight to the hardware for output
    i2s_write(I2S_PORT_SPK, payload, length, &bytes_written, portMAX_DELAY);
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

  // Run initial hardware boot sequence before binding drivers
  init_audio_codecs();
  setup_audio_pipeline();

  webSocket.begin(server_ip, server_port, "/ws/voice"); 
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
}

void loop() {
  webSocket.loop();
  
  // High-frequency polling loop for real-time speech capture
  uint8_t mic_buffer[512]; 
  size_t bytes_read;
  
  i2s_read(I2S_PORT_MIC, mic_buffer, sizeof(mic_buffer), &bytes_read, portMAX_DELAY);
  
  if (bytes_read > 0 && webSocket.isConnected()) {
    webSocket.sendBIN(mic_buffer, bytes_read);
  }
}
