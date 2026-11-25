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
#include <tiny_websockets/network/esp32/esp32_tcp.hpp>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <driver/i2s.h>
#include <memory>
#include <math.h>

using namespace websockets;

// SSL TcpClient for Render (wss:// on port 443) without hardcoded CA expiry issues
class InsecureSslTcpClient : public websockets::network::SecuredEsp32TcpClient {
public:
  InsecureSslTcpClient() {
    this->client.setInsecure();
  }
};

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
#define SAMPLE_RATE     16000
#define MIC_BUF_LEN     512     // Mono samples per WS packet (32ms)
#define AUDIO_RING_SIZE 61440   // 60 KB RAM ring buffer (~1.92s of 16kHz audio)
#define PREBUFFER_BYTES 20480   // Buffer 20 KB (~640ms) before starting speaker

// ─────────────────── CONFIG DEFAULTS ────────────────────────
#define AP_NAME        "Jarvis-Setup"
#define AP_PASS        "12345678"
#define SERVER_IP_MAX  64
#define RESET_HOLD_MS  7000   // Hold BOOT button 7 seconds -> Open WiFi Setup AP

// ─────────────────── GLOBALS ────────────────────────────────
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
std::unique_ptr<WebsocketsClient> ws;
Preferences prefs;

bool wsConnected = false;
bool oledFound = false;
char serverIP[SERVER_IP_MAX] = "ai-wur3.onrender.com";
char serverPort[8]           = "443";
unsigned long lastWsAttempt   = 0;
unsigned long lastWifiAttempt = 0;

enum State { IDLE, RECORDING, WAITING, PLAYING };
volatile State state = IDLE;
volatile bool aiLedState = false;
volatile int speakerVolume = 80;       // 0..100%
volatile uint16_t volumeScale = 183;   // 0..256 fixed-point perceptual gain

// Audio Jitter / Ring Buffer Globals (Dual-task safe)
portMUX_TYPE ringMux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t audioRing[AUDIO_RING_SIZE];
volatile size_t ringHead = 0;
volatile size_t ringTail = 0;
volatile size_t ringCount = 0;
volatile bool playbackStarted = false;
volatile bool audioStreamEnded = false;
volatile bool oledReadyPending = false;
volatile uint32_t underrunCount = 0;

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

void normalizeServerConfig() {
  String host = String(serverIP);
  host.trim();
  bool isHttps = host.startsWith("https://") || host.startsWith("wss://");
  if (host.startsWith("https://")) host.remove(0, 8);
  else if (host.startsWith("http://")) host.remove(0, 7);
  else if (host.startsWith("wss://")) host.remove(0, 6);
  else if (host.startsWith("ws://")) host.remove(0, 5);

  int slashIdx = host.indexOf('/');
  if (slashIdx != -1) host = host.substring(0, slashIdx);

  int colonIdx = host.indexOf(':');
  if (colonIdx != -1) {
    String portPart = host.substring(colonIdx + 1);
    host = host.substring(0, colonIdx);
    if (portPart.length() > 0) {
      portPart.toCharArray(serverPort, 8);
    }
  }

  if (isHttps || host.endsWith(".onrender.com")) {
    if (atoi(serverPort) == 8000 || strlen(serverPort) == 0) {
      strcpy(serverPort, "443");
    }
  }

  host.toCharArray(serverIP, SERVER_IP_MAX);
}

void saveServerConfig() {
  normalizeServerConfig();
  prefs.begin("jarvis", false);
  prefs.putString("ip",   serverIP);
  prefs.putString("port", serverPort);
  prefs.end();
}

void setSpeakerVolume(int pct, bool saveToPrefs = true) {
  pct = constrain(pct, 0, 100);
  speakerVolume = pct;
  // Perceptual loudness curve: pow(pct/100, 1.5) mapped to 0..256
  float gain = powf((float)pct / 100.0f, 1.5f);
  volumeScale = (uint16_t)roundf(gain * 256.0f);
  if (saveToPrefs) {
    prefs.begin("jarvis", false);
    prefs.putInt("vol", pct);
    prefs.end();
  }
  Serial.printf("[Audio] Volume set to %d%% (scale=%u/256)\n", pct, (unsigned)volumeScale);
}

void loadSpeakerVolume() {
  prefs.begin("jarvis", true);
  int pct = prefs.getInt("vol", 80);
  prefs.end();
  setSpeakerVolume(pct, false);
}

void loadServerConfig() {
  prefs.begin("jarvis", true);
  String ip   = prefs.getString("ip",   serverIP);
  String port = prefs.getString("port", serverPort);
  prefs.end();
  if (ip == "192.168.1.100" || ip.length() == 0) {
    ip = "ai-wur3.onrender.com";
    port = "443";
  }
  ip.toCharArray(serverIP, SERVER_IP_MAX);
  port.toCharArray(serverPort, 8);
  normalizeServerConfig();
}

// ════════════════════════════════════════════════════════════
//  WIFIMANAGER SETUP (Boot + On-Demand 7s Reset Portal)
// ════════════════════════════════════════════════════════════
void configureWiFiManager(WiFiManager& wm, WiFiManagerParameter& p_ip, WiFiManagerParameter& p_port) {
  wm.setTitle("Jarvis WiFi Setup");
  wm.setDarkMode(true);
  wm.setConnectTimeout(20);
  wm.setConnectRetries(3);
  wm.setCleanConnect(true);
  wm.addParameter(&p_ip);
  wm.addParameter(&p_port);

  wm.setAPCallback([](WiFiManager* wm) {
    Serial.println(F("========================================"));
    Serial.println(F("  Jarvis Setup Hotspot Active!"));
    Serial.printf ("  SSID: %s | PASS: %s\n", AP_NAME, AP_PASS);
    Serial.println(F("  Open browser: http://192.168.4.1"));
    Serial.println(F("========================================"));
    char apLine[32], passLine[32];
    snprintf(apLine, sizeof(apLine), "WiFi: %s", AP_NAME);
    snprintf(passLine, sizeof(passLine), "Pass: %s", AP_PASS);
    updateOled("Setup Hotspot ON", apLine, passLine, "Open: 192.168.4.1");
  });
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  loadServerConfig();

  WiFiManager wm;
  WiFiManagerParameter p_ip("server_ip", "Server Host / IP", serverIP, SERVER_IP_MAX);
  WiFiManagerParameter p_port("server_port", "Server Port", serverPort, 8);
  configureWiFiManager(wm, p_ip, p_port);

  // Check if BOOT button held at power-on
  pinMode(BTN_PIN, INPUT_PULLUP);
  delay(100);
  if (digitalRead(BTN_PIN) == LOW) {
    Serial.println(F("[WiFi] BOOT button held at boot - resetting WiFi settings..."));
    updateOled("WiFi Reset!", "Clearing saved WiFi", "Starting Setup AP...");
    blinkLED(6, 60);
    wm.resetSettings();
  }

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
  char srvLine[80];
  snprintf(srvLine, sizeof(srvLine), "Srv: %s:%s", serverIP, serverPort);
  updateOled("WiFi Connected!", ipLine, srvLine, "Connecting Server...");

  blinkLED(3, 120);
}

void openWiFiSetupPortal() {
  Serial.println(F("[Setup] Launching WiFi Setup Portal (AP Mode)..."));
  if (ws) {
    ws->close();
    ws.reset();
  }
  wsConnected = false;
  state = IDLE;

  blinkLED(5, 70);
  updateOled("WiFi Setup Mode", "Clearing old WiFi...", "Starting Hotspot...");

  // Reset server config to Render default if user resets
  strcpy(serverIP, "ai-wur3.onrender.com");
  strcpy(serverPort, "443");

  WiFiManager wm;
  wm.resetSettings();
  WiFi.disconnect(true, true);
  delay(300);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  WiFiManagerParameter p_ip("server_ip", "Server Host / IP", serverIP, SERVER_IP_MAX);
  WiFiManagerParameter p_port("server_port", "Server Port", serverPort, 8);
  configureWiFiManager(wm, p_ip, p_port);

  // Blocks until user enters new SSID/password and ESP32 connects, then auto-closes AP & portal!
  if (wm.startConfigPortal(AP_NAME, AP_PASS)) {
    String newIP   = p_ip.getValue();
    String newPort = p_port.getValue();
    if (newIP.length() > 0)   newIP.toCharArray(serverIP, SERVER_IP_MAX);
    if (newPort.length() > 0) newPort.toCharArray(serverPort, 8);
    saveServerConfig();

    char ipLine[32];
    snprintf(ipLine, sizeof(ipLine), "IP: %s", WiFi.localIP().toString().c_str());
    updateOled("WiFi Connected!", ipLine, "Portal Closed!", "Connecting Server...");
    Serial.println(F("[Setup] New WiFi connected & portal closed!"));
    blinkLED(3, 120);
    lastWsAttempt = 0; // Connect immediately
  } else {
    updateOled("Setup Cancelled", "Restarting ESP32...");
    delay(1500);
    ESP.restart();
  }
}

// ════════════════════════════════════════════════════════════
//  I2S MIC & SPEAKER SETUP
// ════════════════════════════════════════════════════════════
void setupMic() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT, // Read both slots (fixes ESP32 IDF L/R swap!)
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = 256,
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
    .dma_buf_len          = 512,  // 256ms hardware DMA buffer (smooth!)
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
//  AUDIO RING BUFFER & DEDICATED FREERTOS PLAYBACK TASK
// ════════════════════════════════════════════════════════════
void resetAudioRing() {
  portENTER_CRITICAL(&ringMux);
  ringHead = 0;
  ringTail = 0;
  ringCount = 0;
  playbackStarted = false;
  audioStreamEnded = false;
  underrunCount = 0;
  portEXIT_CRITICAL(&ringMux);
}

void pushAudioRing(const uint8_t* data, size_t len) {
  size_t offset = 0;
  while (offset < len && state == PLAYING) {
    portENTER_CRITICAL(&ringMux);
    size_t freeSpace = AUDIO_RING_SIZE - ringCount;
    if (freeSpace > 0) {
      size_t toWrite = len - offset;
      if (toWrite > freeSpace) toWrite = freeSpace;
      size_t firstPart = AUDIO_RING_SIZE - ringHead;
      if (firstPart > toWrite) firstPart = toWrite;

      memcpy(audioRing + ringHead, data + offset, firstPart);
      if (toWrite > firstPart) {
        memcpy(audioRing, data + offset + firstPart, toWrite - firstPart);
      }
      ringHead = (ringHead + toWrite) % AUDIO_RING_SIZE;
      ringCount += toWrite;
      offset += toWrite;
    }
    portEXIT_CRITICAL(&ringMux);

    if (offset < len) {
      // Buffer temporarily full (3s ahead); wait briefly for playback task to drain
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
}

void audioPlaybackTask(void* param) {
  static uint8_t txChunk[2048];
  for (;;) {
    if (state != PLAYING) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    size_t countSnap = 0;
    bool endedSnap = false;
    bool startedSnap = false;

    portENTER_CRITICAL(&ringMux);
    countSnap = ringCount;
    endedSnap = audioStreamEnded;
    if (!playbackStarted && (countSnap >= PREBUFFER_BYTES || (endedSnap && countSnap > 0))) {
      playbackStarted = true;
    }
    startedSnap = playbackStarted;
    portEXIT_CRITICAL(&ringMux);

    if (!startedSnap) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    if (countSnap == 0) {
      if (endedSnap) {
        vTaskDelay(pdMS_TO_TICKS(280)); // Drain 256ms hardware DMA tail cleanly
        i2s_zero_dma_buffer(I2S_SPK_PORT);
        uint32_t drops = underrunCount;
        resetAudioRing();
        state = IDLE;
        digitalWrite(LED_PIN, aiLedState ? HIGH : LOW);
        oledReadyPending = true;
        Serial.printf("[Audio] Playback complete. Underruns=%lu\n", (unsigned long)drops);
      } else {
        underrunCount++;
        vTaskDelay(pdMS_TO_TICKS(4));
      }
      continue;
    }

    size_t toCopy = (countSnap > sizeof(txChunk)) ? sizeof(txChunk) : countSnap;
    if (toCopy >= 2) toCopy &= ~((size_t)1); // Keep 16-bit PCM sample alignment
    portENTER_CRITICAL(&ringMux);
    size_t firstPart = AUDIO_RING_SIZE - ringTail;
    if (firstPart > toCopy) firstPart = toCopy;
    memcpy(txChunk, audioRing + ringTail, firstPart);
    if (toCopy > firstPart) {
      memcpy(txChunk + firstPart, audioRing, toCopy - firstPart);
    }
    ringTail = (ringTail + toCopy) % AUDIO_RING_SIZE;
    ringCount -= toCopy;
    portEXIT_CRITICAL(&ringMux);

    // Apply digital volume scaling (0..256 fixed-point)
    uint16_t scale = volumeScale;
    if (scale == 0) {
      memset(txChunk, 0, toCopy);
    } else if (scale != 256) {
      int16_t* samples = (int16_t*)txChunk;
      size_t numSamples = toCopy / 2;
      for (size_t i = 0; i < numSamples; i++) {
        samples[i] = (int16_t)(((int32_t)samples[i] * scale) >> 8);
      }
    }

    size_t written = 0;
    i2s_write(I2S_SPK_PORT, txChunk, toCopy, &written, portMAX_DELAY);
  }
}

// ════════════════════════════════════════════════════════════
//  WEBSOCKET CLIENT
// ════════════════════════════════════════════════════════════
void onWsMessage(WebsocketsMessage msg) {
  if (msg.isBinary()) {
    if (state == PLAYING) {
      pushAudioRing((const uint8_t*)msg.c_str(), msg.length());
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
    oledReadyPending = false;
    resetAudioRing();
    i2s_zero_dma_buffer(I2S_SPK_PORT);
    state = PLAYING;
    updateOled("Speaking...", textSnippet, "Buffering audio...", aiLedState ? "LED: ON" : "LED: OFF");
  } else if (strcmp(type, "audio_end") == 0) {
    Serial.println(F("[Audio] Stream received completely. Draining buffer..."));
    portENTER_CRITICAL(&ringMux);
    audioStreamEnded = true;
    if (!playbackStarted && ringCount > 0) {
      playbackStarted = true;
    }
    portEXIT_CRITICAL(&ringMux);
  } else if (strcmp(type, "iot") == 0) {
    const char* action = doc["action"] | "";
    if (strcmp(action, "led") == 0) {
      const char* ledstate = doc["state"] | "off";
      bool on = (strcmp(ledstate, "on") == 0);
      ledSet(on);
      Serial.printf("[IoT] LED State -> %s\n", ledstate);
      char foot[32];
      snprintf(foot, sizeof(foot), "LED:%s | Vol:%d%%", on ? "ON" : "OFF", (int)speakerVolume);
      updateOled(on ? "IoT: LED ON" : "IoT: LED OFF", "Updated by AI", "Ready", foot);
    } else if (strcmp(action, "volume") == 0) {
      int level = doc["level"] | 80;
      setSpeakerVolume(level, true);
      char volLine[32], foot[32];
      snprintf(volLine, sizeof(volLine), "Level: %d%%", (int)speakerVolume);
      snprintf(foot, sizeof(foot), "LED:%s | Vol:%d%%", aiLedState ? "ON" : "OFF", (int)speakerVolume);
      updateOled("Speaker Volume", volLine, "Saved to ESP32", foot);
    }
  }
}

void onWsEvent(WebsocketsEvent event, String data) {
  if (event == WebsocketsEvent::ConnectionOpened) {
    wsConnected = true;
    Serial.println(F("[WS] Connected to Jarvis Server!"));
    char foot[32];
    snprintf(foot, sizeof(foot), "LED:%s | Vol:%d%%", aiLedState ? "ON" : "OFF", (int)speakerVolume);
    updateOled("Server Connected!", "Status: Ready", "Hold BOOT to talk", foot);
    blinkLED(2, 120);
  } else if (event == WebsocketsEvent::ConnectionClosed) {
    wsConnected = false;
    state = IDLE;
    resetAudioRing();
    i2s_zero_dma_buffer(I2S_SPK_PORT);
    Serial.println(F("[WS] Connection lost."));
    updateOled("Server Offline", "Retrying connection...", serverIP, "Status: Reconnecting");
  } else if (event == WebsocketsEvent::GotPing) {
    if (ws) ws->pong();
  }
}

void connectWS() {
  lastWsAttempt = millis();
  normalizeServerConfig();
  int port = atoi(serverPort);
  bool useSSL = (port == 443);

  Serial.printf("[WS] Connecting (%s) to %s:%d/ws...\n", useSSL ? "wss" : "ws", serverIP, port);

  ws.reset(useSSL
    ? new WebsocketsClient(std::make_shared<InsecureSslTcpClient>())
    : new WebsocketsClient());

  ws->onMessage(onWsMessage);
  ws->onEvent(onWsEvent);

  char statusLine[80];
  snprintf(statusLine, sizeof(statusLine), "%s:%d", serverIP, port);
  updateOled(useSSL ? "Connecting WSS..." : "Connecting WS...", statusLine, "Please wait...", "Hold BOOT 7s=Setup");

  if (!ws->connect(serverIP, port, "/ws")) {
    Serial.println(F("[WS] Failed to connect. Will retry in 4s..."));
    updateOled("WS Connect Failed", statusLine, "Retrying in 4s...", "Hold BOOT 7s=Setup");
  }
}

// ════════════════════════════════════════════════════════════
//  AUDIO RECORDING & TRANSMISSION (Hold to speak)
//  Returns false if held >= 7 seconds (WiFi Setup Reset trigger)
// ════════════════════════════════════════════════════════════
bool recordAndSend() {
  if (!wsConnected || !ws) {
    return true;
  }

  Serial.println(F("[MIC] Recording started... (Release BOOT to send, hold 7s = WiFi Setup)"));
  state = RECORDING;
  digitalWrite(LED_PIN, HIGH);
  updateOled("Listening...", "Speak into mic...", "Release BOOT to send", "Hold 7s = WiFi Setup");

  ws->send("{\"type\":\"start_audio\"}");

  // Static buffers keep loopTask stack safe alongside WiFiClientSecure TLS
  static int32_t rawBuf[MIC_BUF_LEN * 2];
  static int16_t pcmBuf[MIC_BUF_LEN];

  // Flush initial DMA startup transient
  size_t bytesRead = 0;
  i2s_zero_dma_buffer(I2S_MIC_PORT);
  i2s_read(I2S_MIC_PORT, rawBuf, sizeof(rawBuf), &bytesRead, 100 / portTICK_PERIOD_MS);

  int32_t dc0 = 0, dc1 = 0;
  bool dcInit = false;
  int32_t sessionMaxPeak = 0;
  int chunkCount = 0;
  unsigned long recStart = millis();

  while (digitalRead(BTN_PIN) == LOW) {
    if (millis() - recStart >= RESET_HOLD_MS) {
      ws->send("{\"type\":\"end_audio\"}");
      digitalWrite(LED_PIN, aiLedState ? HIGH : LOW);
      state = IDLE;
      return false; // Trigger 7s WiFi Setup Portal!
    }

    bytesRead = 0;
    i2s_read(I2S_MIC_PORT, rawBuf, sizeof(rawBuf), &bytesRead, portMAX_DELAY);
    int frames = bytesRead / 8; // Stereo 32-bit = 8 bytes per frame
    if (frames <= 0) continue;

    // Shift >> 14 preserves 18-bit headroom and prevents DC clipping
    if (!dcInit) {
      int64_t sum0 = 0, sum1 = 0;
      for (int i = 0; i < frames; i++) {
        sum0 += (rawBuf[2 * i] >> 14);
        sum1 += (rawBuf[2 * i + 1] >> 14);
      }
      dc0 = (int32_t)(sum0 / frames);
      dc1 = (int32_t)(sum1 / frames);
      dcInit = true;
    }

    // Pick whichever I2S slot (L or R) actually carries audio variation
    int32_t min0 = INT32_MAX, max0 = INT32_MIN;
    int32_t min1 = INT32_MAX, max1 = INT32_MIN;
    for (int i = 0; i < frames; i++) {
      int32_t s0 = rawBuf[2 * i] >> 14;
      int32_t s1 = rawBuf[2 * i + 1] >> 14;
      if (s0 < min0) min0 = s0;
      if (s0 > max0) max0 = s0;
      if (s1 < min1) min1 = s1;
      if (s1 > max1) max1 = s1;
    }
    int32_t range0 = max0 - min0;
    int32_t range1 = max1 - min1;
    int activeSlot = (range1 > range0) ? 1 : 0;

    int32_t chunkPeak = 0;
    for (int i = 0; i < frames; i++) {
      int32_t rawVal = rawBuf[2 * i + activeSlot] >> 14;
      int32_t& dc = (activeSlot == 0) ? dc0 : dc1;
      dc = (dc * 63 + rawVal) / 64;   // Remove INMP441 DC offset
      int32_t ac = (rawVal - dc) * 4; // Clean voice gain after DC removal
      ac = constrain(ac, -32768, 32767);
      pcmBuf[i] = (int16_t)ac;
      int32_t a = abs(ac);
      if (a > chunkPeak) chunkPeak = a;
    }
    if (chunkPeak > sessionMaxPeak) sessionMaxPeak = chunkPeak;

    chunkCount++;
    if (chunkCount % 6 == 1) {
      Serial.printf("[MIC] Slot=%d R0=%ld R1=%ld Peak=%ld\n",
                    activeSlot, (long)range0, (long)range1, (long)chunkPeak);
    }

    ws->sendBinary((const char*)pcmBuf, frames * 2);
    ws->poll();
  }

  ws->send("{\"type\":\"end_audio\"}");
  digitalWrite(LED_PIN, aiLedState ? HIGH : LOW);

  Serial.printf("[MIC] Done (%d chunks, maxPeak=%ld)\n", chunkCount, (long)sessionMaxPeak);
  if (sessionMaxPeak < 80) {
    state = IDLE;
    Serial.println(F("[MIC] WARNING: Zero/silent mic signal! Check SD=32 SCK=26 WS=25 VDD=3.3V"));
    updateOled("No Mic Signal!", "Peak=0 (Silence)", "Check D32/D26/D25", "VDD=3.3V L/R=GND");
    return true;
  }

  state = WAITING;
  updateOled("Thinking...", "Processing audio...", "Whisper + Groq AI");
  return true;
}

// ════════════════════════════════════════════════════════════
//  7-SECOND BOOT BUTTON HOLD CHECK (Works anytime!)
// ════════════════════════════════════════════════════════════
void checkHoldForWiFiSetup() {
  if (digitalRead(BTN_PIN) != LOW) return;
  delay(40); // Debounce
  if (digitalRead(BTN_PIN) != LOW) return;

  // If online & ready, record voice normally (holding >= 7s cancels & opens WiFi setup)
  if (WiFi.status() == WL_CONNECTED && wsConnected && state == IDLE) {
    bool normalFinish = recordAndSend();
    if (!normalFinish) {
      openWiFiSetupPortal();
    }
    return;
  }

  // If offline (WiFi lost or WS failed), immediately show 7-second countdown on OLED!
  unsigned long holdStart = millis();
  int lastSecShown = -1;
  while (digitalRead(BTN_PIN) == LOW) {
    unsigned long elapsed = millis() - holdStart;
    if (elapsed >= RESET_HOLD_MS) {
      openWiFiSetupPortal();
      return;
    }
    int secLeft = (int)((RESET_HOLD_MS - elapsed + 999) / 1000);
    if (secLeft != lastSecShown) {
      lastSecShown = secLeft;
      char countLine[32];
      snprintf(countLine, sizeof(countLine), "Keep holding: %ds...", secLeft);
      updateOled("WiFi Setup Trigger", countLine, "Creates Jarvis-Setup", "Release to cancel");
      Serial.printf("[Setup] %s\n", countLine);
    }
    delay(50);
  }

  // Released early before 7 seconds
  updateOled("Setup Cancelled", "Resuming...", serverIP);
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
  loadSpeakerVolume();
  setupMic();
  setupSpeaker();
  xTaskCreatePinnedToCore(audioPlaybackTask, "AudioPlay", 4096, NULL, 2, NULL, 1);
  connectWS();

  Serial.println(F("[Ready] Hold BOOT to talk | Hold 7s for WiFi Setup!"));
}

void loop() {
  // Safe OLED update triggered by background AudioPlay task when speech finishes
  if (oledReadyPending) {
    oledReadyPending = false;
    char foot[32];
    snprintf(foot, sizeof(foot), "LED:%s | Vol:%d%%", aiLedState ? "ON" : "OFF", (int)speakerVolume);
    updateOled("Status: Ready", "Hold BOOT to talk", "Server: Connected", foot);
  }

  // 1. Always check BOOT button FIRST (works even if WiFi/WS is offline!)
  checkHoldForWiFiSetup();

  // 2. Non-blocking WiFi reconnect every 8s if dropped
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiAttempt >= 8000 || lastWifiAttempt == 0) {
      lastWifiAttempt = millis();
      Serial.println(F("[WiFi] Disconnected! Reconnecting..."));
      updateOled("WiFi Lost!", "Reconnecting...", "Hold BOOT 7s=Setup");
      blinkLED(2, 80);
      WiFi.reconnect();
    }
    delay(20);
    return;
  }

  // 3. Non-blocking WebSocket reconnect every 4s if offline
  if (!wsConnected) {
    if (millis() - lastWsAttempt >= 4000 || lastWsAttempt == 0) {
      connectWS();
    }
    delay(20);
    return;
  }

  if (ws) ws->poll();
  delay(state == PLAYING ? 1 : 10);
}
