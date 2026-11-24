# 🤖 JARVIS ESP32 — PlatformIO Project

Yeh project VS Code + PlatformIO ke liye tayyar kiya gaya hai, jisme aapke handwritten pinout aur OLED display ka pura support shamil hai.

---

## 📌 Pinout Connections (Exact as requested)

| Component | Pin Name | ESP32 GPIO Pin |
|---|---|---|
| **LED** | `+ LED` | **GPIO 18** (`D18`) |
| **OLED Display (I2C)** | `SDA` | **GPIO 4** (`D4`) |
| | `SCL` | **GPIO 15** (`D15`) |
| | `VCC` | `3.3V` |
| | `GND` | `GND` |
| **I2S Mic (INMP441)** | `SD` (Data Out) | **GPIO 32** (`D32`) |
| | `SCK` (BCLK) | **GPIO 26** (`D26`) |
| | `WS` (LRCLK) | **GPIO 25** (`D25`) |
| | `L/R` | `GND` |
| | `VDD` | `3.3V` |
| | `GND` | `GND` |
| **I2S Amp (MAX98357A)**| `BCLK` | **GPIO 14** (`D14`) |
| | `LRCLK` | **GPIO 27** (`D27`) |
| | `DIN` | **GPIO 33** (`D33`) |
| | `VIN` | `5V` (or `3.3V`) |
| | `GND` | `GND` |
| **Push to Talk Button** | **BOOT Button** | **GPIO 0** (ESP32 on-board button) |

> 💡 **Button Note:** ESP32 ke upar jo built-in **BOOT button** hota hai, wahi Push-to-Talk button banaya gaya hai. Alag se koi button jodne ki jarurat nahi hai!
> - **Baat karne ke liye:** BOOT button ko press karke rakhein aur bolein, bolna khatam hote hi chhod dein.
> - **WiFi Reset karne ke liye:** BOOT button ko 6 second tak lagataar daba ke rakhein.

---

## 🚀 PlatformIO se Upload kaise karein:

1. **VS Code open karein**.
2. **PlatformIO IDE extension** install hona chahiye (agar nahi hai to VS Code Extensions tab me `PlatformIO IDE` search karke install karein).
3. **Open Project:**
   - PlatformIO Home icon par click karein.
   - **Open Project** par click karke folder select karein:  
     `d:\AI\esp32_platformio`
4. ESP32 ko USB cable se laptop/PC se connect karein.
5. Bottom blue bar me **Upload** icon (`→`) ya `Ctrl+Alt+U` dabayein.
6. Libraries (`WiFiManager`, `ArduinoWebsockets`, `ArduinoJson`, `Adafruit SSD1306`) PlatformIO automatically download aur compile kar lega!

---

## 🌐 First Time WiFi Setup (Captive Portal):
1. Pehli baar boot hone par OLED par **"Hotspot Setup AP"** dikhega.
2. Apne phone ya laptop se connect karein:
   - **WiFi Name (SSID):** `Jarvis-Setup`
   - **Password:** `12345678`
3. Browser me automatically setup page open hoga (ya `http://192.168.4.1` kholein).
4. Apna Home WiFi select karein, Password dalein.
5. **Server Host/IP** aur **Port** enter karke Save karein:
   - Local: Jaise `192.168.1.15` aur Port `8000`
   - Render: Jaise `your-app.onrender.com` aur Port `443`
6. ESP32 connect hote hi OLED screen par **"WiFi Connected! Status: Ready"** aa jaega!
