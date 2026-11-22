# Jarvis AI Server — Python + ESP32

## Project Structure
```
AI/
├── server/                        ← FastAPI backend (deploy on Render)
│   ├── gemini-code-1795329199931.py  ← Main server
│   ├── requirements.txt
│   └── .env.example
├── esp32/
│   └── jarvis_esp32/
│       └── jarvis_esp32.ino       ← ESP32 Arduino code
└── render.yaml                    ← Render deploy config
```

## Server Setup (Local)
```bash
cd server
pip install -r requirements.txt
cp .env.example .env
# Edit .env and add your GROQ_API_KEY
uvicorn gemini-code-1795329199931:app --host 0.0.0.0 --port 8000
```

## Deploy on Render
1. Fork/connect this repo on [render.com](https://render.com)
2. Add Environment Variable: `GROQ_API_KEY = your_key`
3. Build & deploy!

## ESP32
- Open `esp32/jarvis_esp32/jarvis_esp32.ino` in Arduino IDE
- Install libraries: WiFiManager, ArduinoWebsockets, ArduinoJson
- Upload to ESP32
- On first boot: connect to `Jarvis-Setup` WiFi → configure

## Get Free Groq API Key
👉 https://console.groq.com
