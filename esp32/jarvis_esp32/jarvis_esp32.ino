/*
  ================================================================
   JARVIS ESP32 — AI Voice Assistant (WiFiManager + WebSocket)
  ================================================================
  Hardware:
    - ESP32 DevKit
    - INMP441  → I2S Microphone
    - MAX98357A → I2S Speaker
    - Push Button  GPIO 15  (hold to speak)
    - Reset Button GPIO 14  (hold 3s to clear WiFi & reconfigure)
    - LED          GPIO  2  (built-in)

  First Boot / WiFi Reset Flow:
    1. ESP apna AP banata hai: "Jarvis-Setup"
    2. Phone se "Jarvis-Setup" se connect karo
    3. Browser mein http://192.168.4.1 khulega (auto captive portal)
    4. WiFi SSID select karo, password + Server IP dalo → Save
    5. ESP restart hoga aur apne aap connect ho jaega
    6. Credentials EEPROM/Flash mein save rehte hain

  Libraries (Arduino Library Manager se install karo):
    ✅ WiFiManager          — by tzapu (search: "WiFiManager")
    ✅ ArduinoWebsockets    — by Gil Maimon
    ✅ ArduinoJson          — by Benoit Blanchon
    ✅ Preferences          — built-in ESP32 core
    ✅ driver/i2s.h         — built-in ESP32 core
  ================================================================
*/

#include <WiFi.h>
#include <WiFiManager.h>          // tzapu/WiFiManager
#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <driver/i2s.h>

using namespace websockets;

// ─────────────────── PINS ────────────────────────────
#define BTN_PIN      15    // Press & hold = Speak
#define RESET_PIN    14    // Hold 3s = Clear WiFi config
#define LED_PIN       2    // Built-in LED / AI-controlled LED

// ─────────────────── I2S MIC (INMP441) ───────────────
#define I2S_MIC_PORT I2S_NUM_0
#define MIC_SCK      26
#define MIC_WS       25
#define MIC_DIN      32

// ─────────────────── I2S SPEAKER (MAX98357A) ─────────
#define I2S_SPK_PORT I2S_NUM_1
#define SPK_SCK      26   // Shared with mic clock
#define SPK_WS       25
#define SPK_DOUT     22

// ─────────────────── AUDIO CONFIG ────────────────────
#define SAMPLE_RATE   16000
#define MIC_BUF_LEN   256   // Samples per chunk

// ─────────────────── DEFAULTS ────────────────────────
#define AP_NAME        "Jarvis-Setup"   // Hotspot name user dikhega
#define AP_PASS        "12345678"       // Hotspot password (min 8 chars)
#define DEFAULT_PORT   "8000"
#define SERVER_IP_MAX  40

// ─────────────────── GLOBALS ─────────────────────────
WebsocketsClient ws;
Preferences prefs;
bool wsConnected = false;
char serverIP[SERVER_IP_MAX]   = "192.168.1.100";
char serverPort[6]             = "8000";

enum State { IDLE, RECORDING, WAITING, PLAYING };
State state = IDLE;
bool aiLedState = false;   // Track LED state from AI command


// ══════════════════════════════════════════════════════
//  HELPERS
// ══════════════════════════════════════════════════════
void blinkLED(int times, int ms = 120) {
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
  ip.toCharArray(serverIP,   SERVER_IP_MAX);
  port.toCharArray(serverPort, 6);
}


// ══════════════════════════════════════════════════════
//  WIFIMANAGER SETUP
//  → Custom parameters: Server IP + Port
//  → Reset button: hold RESET_PIN LOW for 3s
// ══════════════════════════════════════════════════════
void setupWiFi() {
  loadServerConfig();

  WiFiManager wm;
  wm.setTitle("Jarvis Configuration");
  wm.setDarkMode(true);

  // Custom parameters shown on portal
  WiFiManagerParameter p_ip(
    "server_ip", "Server IP Address", serverIP, SERVER_IP_MAX
  );
  WiFiManagerParameter p_port(
    "server_port", "Server Port", serverPort, 6
  );
  wm.addParameter(&p_ip);
  wm.addParameter(&p_port);

  // Check if RESET_PIN is held LOW → force portal
  pinMode(RESET_PIN, INPUT_PULLUP);
  delay(100);
  if (digitalRead(RESET_PIN) == LOW) {
    Serial.println("[WiFi] Reset button dabaya — config clear ho raha hai!");
    blinkLED(5, 80);
    wm.resetSettings();
  }

  // Captive-portal callback: LED fast blink while portal open
  wm.setAPCallback([](WiFiManager* wm) {
    Serial.println("╔══════════════════════════════════╗");
    Serial.println("║  Jarvis-Setup AP chal raha hai   ║");
    Serial.printf ("║  Connect: %-22s ║\n", AP_NAME);
    Serial.printf ("║  Pass:    %-22s ║\n", AP_PASS);
    Serial.println("║  Browser: http://192.168.4.1     ║");
    Serial.println("╚══════════════════════════════════╝");
    // Fast blink = portal open
    for (int i = 0; i < 6; i++) {
      digitalWrite(LED_PIN, HIGH); delay(60);
      digitalWrite(LED_PIN, LOW);  delay(60);
    }
  });

  // Blocking: connects to saved WiFi OR opens portal
  if (!wm.autoConnect(AP_NAME, AP_PASS)) {
    Serial.println("[WiFi] Connection fail! Restart...");
    delay(2000);
    ESP.restart();
  }

  // Connected — save custom params
  String newIP   = p_ip.getValue();
  String newPort = p_port.getValue();
  if (newIP.length() > 0)   newIP.toCharArray(serverIP, SERVER_IP_MAX);
  if (newPort.length() > 0) newPort.toCharArray(serverPort, 6);
  saveServerConfig();

  Serial.println("[WiFi] Connected!");
  Serial.printf ("[WiFi] IP: %s\n",        WiFi.localIP().toString().c_str());
  Serial.printf ("[Server] %s:%s\n", serverIP, serverPort);
  blinkLED(3, 150);
}


// ══════════════════════════════════════════════════════
//  I2S SETUP
// ══════════════════════════════════════════════════════
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


// ══════════════════════════════════════════════════════
//  WEBSOCKET CALLBACKS
// ══════════════════════════════════════════════════════
void onWsMessage(WebsocketsMessage msg) {
  if (msg.isBinary()) {
    if (state == PLAYING) {
      const uint8_t* data = (const uint8_t*)msg.c_str();
      size_t written = 0;
      i2s_write(I2S_SPK_PORT, data, msg.length(), &written, portMAX_DELAY);
    }
    return;
  }

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, msg.data()) != DeserializationError::Ok) return;
  const char* type = doc["type"] | "";

  if (strcmp(type, "status") == 0) {
    const char* txt = doc["text"] | "";
    Serial.printf("[Status] %s\n", txt);
    if (strcmp(txt, "Thinking...") == 0) {
      state = WAITING;
      // Slow pulse while thinking
      for (int i = 0; i < 2; i++) {
        digitalWrite(LED_PIN, HIGH); delay(300);
        digitalWrite(LED_PIN, LOW);  delay(300);
      }
    } else if (strcmp(txt, "Ready") == 0) {
      state = IDLE;
      digitalWrite(LED_PIN, aiLedState ? HIGH : LOW);
    }

  } else if (strcmp(type, "audio_start") == 0) {
    Serial.printf("[Jarvis] %s...\n", (const char*)(doc["text"] | ""));
    state = PLAYING;
    i2s_zero_dma_buffer(I2S_SPK_PORT);

  } else if (strcmp(type, "audio_end") == 0) {
    Serial.println("[Audio] Done.");
    delay(300);
    i2s_zero_dma_buffer(I2S_SPK_PORT);
    state = IDLE;
    digitalWrite(LED_PIN, aiLedState ? HIGH : LOW);

  } else if (strcmp(type, "iot") == 0) {
    const char* action   = doc["action"] | "";
    const char* ledstate = doc["state"]  | "off";
    if (strcmp(action, "led") == 0) {
      ledSet(strcmp(ledstate, "on") == 0);
      Serial.printf("[IoT] LED → %s\n", ledstate);
    }
  }
}

void onWsEvent(WebsocketsEvent event, String data) {
  if (event == WebsocketsEvent::ConnectionOpened) {
    wsConnected = true;
    Serial.println("[WS] Server connected!");
    blinkLED(2, 150);
  } else if (event == WebsocketsEvent::ConnectionClosed) {
    wsConnected = false;
    state = IDLE;
    Serial.println("[WS] Disconnected!");
  } else if (event == WebsocketsEvent::GotPing) {
    ws.pong();
  }
}

void connectWS() {
  int port = atoi(serverPort);
  Serial.printf("[WS] Connecting to ws://%s:%d/ws\n", serverIP, port);
  ws.onMessage(onWsMessage);
  ws.onEvent(onWsEvent);
  if (!ws.connect(serverIP, port, "/ws")) {
    Serial.println("[WS] Failed. 3s mein retry...");
    delay(3000);
  }
}


// ══════════════════════════════════════════════════════
//  RECORD + SEND (while button held)
// ══════════════════════════════════════════════════════
void recordAndSend() {
  if (!wsConnected) { blinkLED(3, 80); return; }

  Serial.println("[MIC] Recording... (button chhoddo to stop)");
  state = RECORDING;
  digitalWrite(LED_PIN, HIGH);

  // Send start marker
  ws.send("{\"type\":\"start_audio\"}");

  int32_t rawBuf[MIC_BUF_LEN];
  int16_t pcmBuf[MIC_BUF_LEN];

  while (digitalRead(BTN_PIN) == LOW) {
    size_t bytesRead = 0;
    i2s_read(I2S_MIC_PORT, rawBuf, sizeof(rawBuf), &bytesRead, portMAX_DELAY);
    int samples = bytesRead / 4;

    // 32-bit INMP441 → 16-bit PCM conversion + gain
    for (int i = 0; i < samples; i++) {
      int32_t s = rawBuf[i] >> 11;
      s = constrain(s, -32768, 32767);
      pcmBuf[i] = (int16_t)s;
    }

    ws.sendBinary((const char*)pcmBuf, samples * 2);
    ws.poll();
  }

  // Send end marker
  ws.send("{\"type\":\"end_audio\"}");
  digitalWrite(LED_PIN, LOW);
  state = WAITING;
  Serial.println("[MIC] Sent. Jarvis soch raha hai...");
}


// ══════════════════════════════════════════════════════
//  SETUP & LOOP
// ══════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(LED_PIN,   OUTPUT);
  pinMode(BTN_PIN,   INPUT_PULLUP);
  pinMode(RESET_PIN, INPUT_PULLUP);

  Serial.println("\n╔══════════════════════════╗");
  Serial.println("║   JARVIS ESP32  Boot...  ║");
  Serial.println("╚══════════════════════════╝");

  blinkLED(1, 400);

  setupWiFi();      // WiFiManager — auto or portal
  setupMic();
  setupSpeaker();
  connectWS();

  Serial.println("[Ready] Button daba ke baat karo Jarvis se!");
}

void loop() {
  // WiFi drop → reconnect
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Dropped! Reconnecting...");
    blinkLED(3, 100);
    WiFi.reconnect();
    delay(3000);
    return;
  }

  // WS drop → reconnect
  if (!wsConnected) {
    connectWS();
    delay(2000);
    return;
  }

  ws.poll();

  // Speak button
  if (state == IDLE && digitalRead(BTN_PIN) == LOW) {
    delay(30);  // debounce
    if (digitalRead(BTN_PIN) == LOW) {
      recordAndSend();
    }
  }

  // WiFi reset button (hold RESET_PIN 3 seconds)
  if (digitalRead(RESET_PIN) == LOW) {
    Serial.println("[Reset] Button held... 3s mein WiFi clear hoga");
    unsigned long t = millis();
    while (digitalRead(RESET_PIN) == LOW) {
      if (millis() - t > 3000) {
        Serial.println("[Reset] WiFi clear! Restart...");
        blinkLED(5, 60);
        WiFiManager wm;
        wm.resetSettings();
        delay(500);
        ESP.restart();
      }
      delay(50);
    }
  }

  delay(10);
}
