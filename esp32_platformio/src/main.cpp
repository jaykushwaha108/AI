/*
  ================================================================
   JARVIS ESP32 — PlatformIO AI Voice Assistant Client
  ================================================================
   Pinout (Exact as requested):
     - LED                : GPIO 18
     - OLED I2C SDA       : GPIO 4
     - OLED I2C SCL       : GPIO 15
     - I2S Mic (INMP441)  :
         * SCK (BCLK)     : GPIO 26
         * WS  (LRCLK)    : GPIO 25
         * SD  (DOUT)     : GPIO 32
         * L/R            : GND
     - I2S Amp (MAX98357A):
         * BCLK           : GPIO 14
         * LRCLK          : GPIO 27
         * DIN            : GPIO 33
     - Push to Talk Button: GPIO 0 (On-board BOOT button)
         * Hold to Speak
         * Hold 6s to Reset WiFi config

   Features:
     - 0.96" I2C OLED Display (SSD1306 128x64) Live Status
     - Auto Captive Portal (WiFiManager) with saved Server IP/Port
     - Full-duplex WebSocket audio streaming
     - AI IoT Smart LED control
  ================================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <WiFiManager.h>
#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <driver/i2s.h>

using namespace websockets;

// ─────────────────── PIN DEFINITIONS ────────────────────────
#define LED_PIN       18    // Status / AI-controlled LED
#define BTN_PIN        0    // ESP32 On-board BOOT Button (Press & hold = Speak)

// OLED Display (I2C)
#define OLED_SDA       4
#define OLED_SCL      15
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1

// I2S Microphone (INMP441)
#define I2S_MIC_PORT I2S_NUM_0
#define MIC_SCK       26
#define MIC_WS        25
#define MIC_DIN       32

// I2S Amplifier / Speaker (MAX98357A)
#define I2S_SPK_PORT I2S_NUM_1
#define SPK_SCK       14    // BCLK
#define SPK_WS        27    // LRCLK
#define SPK_DOUT      33    // DIN

// ─────────────────── AUDIO CONFIG ───────────────────────────
#define SAMPLE_RATE   16000
#define MIC_BUF_LEN   256

// ─────────────────── CONFIG DEFAULTS ────────────────────────
#define AP_NAME        "Jarvis-Setup"
#define AP_PASS        "12345678"
#define SERVER_IP_MAX  64

// ─────────────────── GLOBALS ────────────────────────────────
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WebsocketsClient ws;
Preferences prefs;

bool wsConnected = false;
bool oledFound = false;
char serverIP[SERVER_IP_MAX] = "192.168.1.100";
char serverPort[8]           = "8000";

enum State { IDLE, RECORDING, WAITING, PLAYING };
State state = IDLE;
bool aiLedState = false;

// ════════════════════════════════════════════════════════════
//  OLED DISPLAY HELPERS
// ════════════════════════════════════════════════════════════
void updateOled(const char* title, const char* line1 = "", const char* line2 = "", const char* line3 = "") {
  if (!oledFound) return;
  display.clearDisplay();

  // Header Bar
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(18, 2);
  display.print(F("JARVIS AI SYSTEM"));
  display.drawLine(0, 12, 127, 12, SSD1306_WHITE);

  // Content lines
  display.setCursor(0, 18);
  display.println(title);

  if (line1 && strlen(line1) > 0) {
    display.setCursor(0, 30);
    display.println(line1);
  }
  if (line2 && strlen(line2) > 0) {
    display.setCursor(0, 42);
    display.println(line2);
  }
  if (line3 && strlen(line3) > 0) {
    display.setCursor(0, 54);
    display.println(line3);
  }

  display.display();
}

void initOled() {
  Wire.begin(OLED_SDA, OLED_SCL);
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    oledFound = true;
  } else if (display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
    oledFound = true;
  }

  if (oledFound) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    updateOled("Booting...", "Initializing hardware", "Please wait...");
  } else {
    Serial.println(F("[OLED] Warning: Display not found on 0x3C or 0x3D!"));
  }
}

// ════════════════════════════════════════════════════════════
//  LED & SETTINGS HELPERS
// ════════════════════════════════════════════════════════════
void blinkLED(int times, int ms = 100) {
  bool prev = aiLedState;
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH); delay(ms);
    digitalWrite(LED_PIN, LOW);  delay(ms);
  }
  digitalWrite(LED_PIN, prev ? HIGH : LOW);
}

void ledSet(bool on) {
  aiLedState = on;
  digitalWrite(LED_PIN, on ? HIGH : LOW);
}

void saveServerConfig() {
  prefs.begin("jarvis", false);
  prefs.putString("ip",   serverIP);
  prefs.putString("port", serverPort);
  prefs.end();
}

void loadServerConfig() {
  prefs.begin("jarvis", true);
  String ip   = prefs.getString("ip",   serverIP);
  String port = prefs.getString("port", serverPort);
  prefs.end();
  ip.toCharArray(serverIP, SERVER_IP_MAX);
  port.toCharArray(serverPort, 8);
}

// ════════════════════════════════════════════════════════════
//  WIFIMANAGER SETUP
// ════════════════════════════════════════════════════════════
void setupWiFi() {
  loadServerConfig();

  WiFiManager wm;
  wm.setTitle("Jarvis Configuration");
  wm.setDarkMode(true);

  WiFiManagerParameter p_ip("server_ip", "Server Host / IP", serverIP, SERVER_IP_MAX);
  WiFiManagerParameter p_port("server_port", "Server Port", serverPort, 8);
  wm.addParameter(&p_ip);
  wm.addParameter(&p_port);

  // Check if BOOT button held at boot
  pinMode(BTN_PIN, INPUT_PULLUP);
  delay(100);
  if (digitalRead(BTN_PIN) == LOW) {
    Serial.println(F("[WiFi] BOOT button held - resetting WiFi settings..."));
    updateOled("WiFi Reset!", "Clearing saved WiFi", "Starting Setup AP...");
    blinkLED(6, 60);
    wm.resetSettings();
  }

  wm.setAPCallback([](WiFiManager* wm) {
    Serial.println(F("========================================"));
    Serial.println(F("  Jarvis-Setup Hotspot Active!"));
    Serial.printf ("  SSID: %s | PASS: %s\n", AP_NAME, AP_PASS);
    Serial.println(F("  Open browser: http://192.168.4.1"));
    Serial.println(F("========================================"));
    updateOled("Hotspot Setup AP", "Connect: Jarvis-Setup", "Pass: 12345678", "Open: 192.168.4.1");
  });

  updateOled("Connecting WiFi...", "Connecting to router...", serverIP);

  if (!wm.autoConnect(AP_NAME, AP_PASS)) {
    Serial.println(F("[WiFi] Failed to connect. Restarting..."));
    updateOled("WiFi Failed!", "Restarting ESP32...");
    delay(2000);
    ESP.restart();
  }

  String newIP   = p_ip.getValue();
  String newPort = p_port.getValue();
  if (newIP.length() > 0)   newIP.toCharArray(serverIP, SERVER_IP_MAX);
  if (newPort.length() > 0) newPort.toCharArray(serverPort, 8);
  saveServerConfig();

  Serial.println(F("[WiFi] Connected successfully!"));
  Serial.printf ("[WiFi] ESP32 IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf ("[Server] Target: %s:%s\n", serverIP, serverPort);

  char ipLine[32];
  snprintf(ipLine, sizeof(ipLine), "IP: %s", WiFi.localIP().toString().c_str());
  char srvLine[32];
  snprintf(srvLine, sizeof(srvLine), "Srv: %s:%s", serverIP, serverPort);
  updateOled("WiFi Connected!", ipLine, srvLine, "Status: Ready");

  blinkLED(3, 120);
}

// ════════════════════════════════════════════════════════════
//  I2S MIC & SPEAKER SETUP
// ════════════════════════════════════════════════════════════
void setupMic() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = 128,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num   = MIC_SCK,
    .ws_io_num    = MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = MIC_DIN
  };
  i2s_driver_install(I2S_MIC_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_MIC_PORT, &pins);
  i2s_zero_dma_buffer(I2S_MIC_PORT);
}

void setupSpeaker() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = 128,
    .use_apll             = false,
    .tx_desc_auto_clear   = true,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num   = SPK_SCK,
    .ws_io_num    = SPK_WS,
    .data_out_num = SPK_DOUT,
    .data_in_num  = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_SPK_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_SPK_PORT, &pins);
  i2s_zero_dma_buffer(I2S_SPK_PORT);
}

// ════════════════════════════════════════════════════════════
//  WEBSOCKET CLIENT
// ════════════════════════════════════════════════════════════
void onWsMessage(WebsocketsMessage msg) {
  if (msg.isBinary()) {
    if (state == PLAYING) {
      const uint8_t* data = (const uint8_t*)msg.c_str();
      size_t written = 0;
      i2s_write(I2S_SPK_PORT, data, msg.length(), &written, portMAX_DELAY);
    }
    return;
  }

  StaticJsonDocument<384> doc;
  if (deserializeJson(doc, msg.data()) != DeserializationError::Ok) return;
  const char* type = doc["type"] | "";

  if (strcmp(type, "status") == 0) {
    const char* txt = doc["text"] | "";
    Serial.printf("[Status] %s\n", txt);
    if (strcmp(txt, "Thinking...") == 0) {
      state = WAITING;
      updateOled("Thinking...", "Processing query...", "Groq AI active", aiLedState ? "LED: ON" : "LED: OFF");
    } else if (strcmp(txt, "Ready") == 0) {
      state = IDLE;
      digitalWrite(LED_PIN, aiLedState ? HIGH : LOW);
      updateOled("Status: Ready", "Hold BOOT to talk", "Server: Connected", aiLedState ? "LED: ON" : "LED: OFF");
    }
  } else if (strcmp(type, "audio_start") == 0) {
    const char* textSnippet = doc["text"] | "";
    Serial.printf("[Jarvis Speaking] %s...\n", textSnippet);
    state = PLAYING;
    i2s_zero_dma_buffer(I2S_SPK_PORT);
    updateOled("Speaking...", textSnippet, "Audio playback", aiLedState ? "LED: ON" : "LED: OFF");
  } else if (strcmp(type, "audio_end") == 0) {
    Serial.println(F("[Audio] Playback complete."));
    delay(200);
    i2s_zero_dma_buffer(I2S_SPK_PORT);
    state = IDLE;
    digitalWrite(LED_PIN, aiLedState ? HIGH : LOW);
    updateOled("Status: Ready", "Hold BOOT to talk", "Server: Connected", aiLedState ? "LED: ON" : "LED: OFF");
  } else if (strcmp(type, "iot") == 0) {
    const char* action   = doc["action"] | "";
    const char* ledstate = doc["state"]  | "off";
    if (strcmp(action, "led") == 0) {
      bool on = (strcmp(ledstate, "on") == 0);
      ledSet(on);
      Serial.printf("[IoT] LED State -> %s\n", ledstate);
      updateOled(on ? "IoT: LED ON" : "IoT: LED OFF", "Updated by AI", "Ready", on ? "LED: ON" : "LED: OFF");
    }
  }
}

void onWsEvent(WebsocketsEvent event, String data) {
  if (event == WebsocketsEvent::ConnectionOpened) {
    wsConnected = true;
    Serial.println(F("[WS] Connected to Jarvis Server!"));
    updateOled("Server Connected!", "Status: Ready", "Hold BOOT to talk", aiLedState ? "LED: ON" : "LED: OFF");
    blinkLED(2, 120);
  } else if (event == WebsocketsEvent::ConnectionClosed) {
    wsConnected = false;
    state = IDLE;
    Serial.println(F("[WS] Connection lost."));
    updateOled("Server Offline", "Retrying connection...", serverIP, "Status: Reconnecting");
  } else if (event == WebsocketsEvent::GotPing) {
    ws.pong();
  }
}

void connectWS() {
  int port = atoi(serverPort);
  Serial.printf("[WS] Connecting to %s:%d/ws...\n", serverIP, port);
  ws.onMessage(onWsMessage);
  ws.onEvent(onWsEvent);

  char statusLine[32];
  snprintf(statusLine, sizeof(statusLine), "%s:%d", serverIP, port);
  updateOled("Connecting WS...", statusLine, "Please wait...");

  if (!ws.connect(serverIP, port, "/ws")) {
    Serial.println(F("[WS] Failed to connect. Will retry..."));
    updateOled("WS Connect Failed", statusLine, "Retrying in 3s...");
    delay(3000);
  }
}

// ════════════════════════════════════════════════════════════
//  AUDIO RECORDING & TRANSMISSION (Hold to speak)
// ════════════════════════════════════════════════════════════
void recordAndSend() {
  if (!wsConnected) {
    blinkLED(3, 80);
    updateOled("Cannot Speak!", "Server not connected", "Check IP/Port");
    return;
  }

  Serial.println(F("[MIC] Recording started... (Release BOOT button to finish)"));
  state = RECORDING;
  digitalWrite(LED_PIN, HIGH);
  updateOled("Listening...", "Release BOOT to send", "Recording Audio...", "Mic: Active");

  ws.send("{\"type\":\"start_audio\"}");

  int32_t rawBuf[MIC_BUF_LEN];
  int16_t pcmBuf[MIC_BUF_LEN];

  while (digitalRead(BTN_PIN) == LOW) {
    size_t bytesRead = 0;
    i2s_read(I2S_MIC_PORT, rawBuf, sizeof(rawBuf), &bytesRead, portMAX_DELAY);
    int samples = bytesRead / 4;

    // 32-bit INMP441 format -> 16-bit PCM conversion with gain
    for (int i = 0; i < samples; i++) {
      int32_t s = rawBuf[i] >> 11;
      s = constrain(s, -32768, 32767);
      pcmBuf[i] = (int16_t)s;
    }

    ws.sendBinary((const char*)pcmBuf, samples * 2);
    ws.poll();
  }

  ws.send("{\"type\":\"end_audio\"}");
  digitalWrite(LED_PIN, aiLedState ? HIGH : LOW);
  state = WAITING;
  Serial.println(F("[MIC] Audio sent! Waiting for response..."));
  updateOled("Thinking...", "Processing audio...", "Whisper + Groq AI");
}

// ════════════════════════════════════════════════════════════
//  MAIN SETUP & LOOP
// ════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(400);

  pinMode(LED_PIN, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);

  Serial.println(F("\n=========================================="));
  Serial.println(F("   JARVIS ESP32 — Booting (PlatformIO)    "));
  Serial.println(F("=========================================="));

  initOled();
  blinkLED(1, 300);

  setupWiFi();
  setupMic();
  setupSpeaker();
  connectWS();

  Serial.println(F("[Ready] Setup complete. Hold BOOT button to talk!"));
}

void loop() {
  // 1. Reconnect WiFi if lost
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[WiFi] Disconnected! Reconnecting..."));
    updateOled("WiFi Lost!", "Reconnecting...", "Please wait");
    blinkLED(2, 100);
    WiFi.reconnect();
    delay(3000);
    return;
  }

  // 2. Reconnect WebSocket if lost
  if (!wsConnected) {
    connectWS();
    delay(2000);
    return;
  }

  ws.poll();

  // 3. Button press handling (BOOT pin = GPIO 0)
  if (state == IDLE && digitalRead(BTN_PIN) == LOW) {
    delay(40); // Debounce
    if (digitalRead(BTN_PIN) == LOW) {
      // Check if user is just tapping or holding for long reset
      unsigned long pressStart = millis();
      bool isReset = false;

      // Start recording immediately
      recordAndSend();

      // If user kept holding for > 6 seconds, trigger WiFi reset
      if (millis() - pressStart > 6000) {
        Serial.println(F("[Reset] Button held > 6s! Resetting WiFi..."));
        updateOled("Resetting WiFi...", "Clearing settings", "Restarting in AP mode");
        blinkLED(6, 60);
        WiFiManager wm;
        wm.resetSettings();
        delay(1000);
        ESP.restart();
      }
    }
  }

  delay(10);
}
