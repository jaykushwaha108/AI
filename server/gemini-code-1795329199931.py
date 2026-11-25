from __future__ import annotations
import os
import io
import re
import json
import wave
import math
import array
import sqlite3
import asyncio
import subprocess
import unicodedata
from datetime import datetime
from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Request
from fastapi.responses import HTMLResponse, StreamingResponse
from groq import Groq
import edge_tts
import imageio_ffmpeg
from dotenv import load_dotenv

load_dotenv()

# ---------------- TEXT CLEANER FOR TTS ----------------
def clean_for_tts(text: str) -> str:
    """Remove emojis, markdown formatting, smart quotes, and other TTS-unfriendly characters."""
    # Remove markdown bold/italic/headers: **text**, *text*, __text__, # heading
    text = re.sub(r'\*{1,3}(.*?)\*{1,3}', r'\1', text)
    text = re.sub(r'_{1,2}(.*?)_{1,2}', r'\1', text)
    text = re.sub(r'^#{1,6}\s*', '', text, flags=re.MULTILINE)

    # Replace smart/curly quotes with normal quotes or remove
    text = text.replace('\u201c', '').replace('\u201d', '')  # " "
    text = text.replace('\u2018', '').replace('\u2019', '')  # ' '
    text = text.replace('\u00ab', '').replace('\u00bb', '')  # « »
    text = text.replace('\u2039', '').replace('\u203a', '')  # ‹ ›

    # Replace ALL hyphen variants between word characters with a space
    # Covers: ASCII hyphen (-), non-breaking hyphen (‑ U+2011), en-dash (–), em-dash (—), etc.
    hyphen_variants = '\\-\u2011\u2010\u2012\u2013\u2014\u2015\ufe58\ufe63\uff0d'
    text = re.sub(rf'(\w)[{hyphen_variants}]+(\w)', r'\1 \2', text)
    # Remove any remaining standalone hyphens/dashes
    text = re.sub(rf'[{hyphen_variants}]', ' ', text)

    # Remove emojis and other non-text Unicode symbols
    cleaned = []
    for ch in text:
        cat = unicodedata.category(ch)
        cp = ord(ch)
        # Keep: letters (L), numbers (N), punctuation (P), spaces (Z), marks (M)
        # Remove: symbols (S) which includes emojis, and control chars (C) except newline
        if cat.startswith(('L', 'N', 'Z', 'M', 'P')) or ch in ('\n', ' ', ',', '.', '!', '?', ':', ';'):
            cleaned.append(ch)
        elif cat.startswith('S') or (0x1F000 <= cp <= 0x1FFFF) or (0x2600 <= cp <= 0x27BF):
            cleaned.append(' ')  # replace emoji with space
    text = ''.join(cleaned)

    # Collapse multiple spaces/newlines
    text = re.sub(r'[ \t]+', ' ', text)
    text = re.sub(r'\n+', ' ', text)
    return text.strip()

app = FastAPI()
groq_client = Groq(api_key=os.getenv("GROQ_API_KEY"))
FFMPEG_PATH = imageio_ffmpeg.get_ffmpeg_exe()
DB_PATH = "assistant.db"

active_esp32: WebSocket | None = None

# Global: last known rate limit info from Groq response headers
rate_limit_info: dict = {}

# ---------------- DATABASE SETUP ----------------
def init_db():
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute("""
        CREATE TABLE IF NOT EXISTS settings (
            key TEXT PRIMARY KEY,
            value TEXT
        )
    """)
    c.execute("""
        CREATE TABLE IF NOT EXISTS messages (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            role TEXT,
            content TEXT,
            timestamp TEXT
        )
    """)
    defaults = {
        "name": "Jarvis",
        "prompt": "Tum ek ultra-smart, friendly aur natural human-like AI companion ho, bilkul ChatGPT aur Gemini ki tarah. Baat karne ka andaaz natural, casual aur empathetic hona chahiye (Hindi/Hinglish). Current facts: Current year 2026 hai. India me 28 states aur 8 UTs hain. Madhya Pradesh ke Chief Minister Dr. Mohan Yadav hain. PM Narendra Modi hain. Agar user tumhe correct kare ya bole galat answer hai to bina behes kiye vinamrata se samjho aur update do. Response short, clear aur natural 1-3 sentences me do.",
        "voice": "hi-IN-SwaraNeural",
        "model": "openai/gpt-oss-120b"
    }
    for k, v in defaults.items():
        c.execute("INSERT OR IGNORE INTO settings (key, value) VALUES (?, ?)", (k, v))
    conn.commit()
    conn.close()

def get_settings():
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    rows = c.execute("SELECT key, value FROM settings").fetchall()
    conn.close()
    return dict(rows)

def save_settings(data: dict):
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    for k, v in data.items():
        c.execute("REPLACE INTO settings (key, value) VALUES (?, ?)", (k, str(v)))
    conn.commit()
    conn.close()

def add_message(role: str, content: str):
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute(
        "INSERT INTO messages (role, content, timestamp) VALUES (?, ?, ?)",
        (role, content, datetime.now().strftime("%H:%M:%S"))
    )
    conn.commit()
    conn.close()

def get_history(limit=12):
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    rows = c.execute(
        "SELECT role, content, timestamp FROM messages ORDER BY id DESC LIMIT ?", (limit,)
    ).fetchall()
    conn.close()
    return [{"role": r[0], "content": r[1], "timestamp": r[2]} for r in reversed(rows)]

def clear_history():
    conn = sqlite3.connect(DB_PATH)
    conn.execute("DELETE FROM messages")
    conn.commit()
    conn.close()

init_db()

# ---------------- AI PIPELINE ----------------
TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "control_led",
            "description": "Call this ONLY when the user explicitly requests to turn ON or OFF a physical room light or LED (e.g. 'light on karo', 'batti bujha do', 'turn off light'). DO NOT call this for general questions.",
            "parameters": {
                "type": "object",
                "properties": {
                    "state": {"type": "string", "enum": ["on", "off"]}
                },
                "required": ["state"]
            }
        }
    }
]

def pcm_to_wav(pcm_bytes: bytes, sample_rate=16000) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_bytes)
    return buf.getvalue()

SILENCE_HALLUCINATIONS = {
    "thank you.", "thank you", "thanks for watching.", "thanks for watching",
    "you", "you.", "bye.", "bye", "subtitles by", "amara.org"
}

def transcribe_audio(pcm_bytes: bytes) -> str:
    usable_len = len(pcm_bytes) - (len(pcm_bytes) % 2)
    if usable_len < 3200:
        return ""
    samples = array.array("h")
    samples.frombytes(pcm_bytes[:usable_len])
    peak = max((abs(s) for s in samples), default=0)
    rms = math.sqrt(sum(s * s for s in samples) / len(samples)) if samples else 0.0
    print(f"[AUDIO] bytes={usable_len}, peak={peak}, rms={rms:.1f}")
    if peak < 150 or rms < 30:
        return ""

    wav_bytes = pcm_to_wav(pcm_bytes[:usable_len])
    resp = groq_client.audio.transcriptions.create(
        file=("audio.wav", wav_bytes),
        model="whisper-large-v3-turbo",
        response_format="text"
    )
    text = resp.strip()
    if rms < 120 and text.lower() in SILENCE_HALLUCINATIONS:
        print(f"[AUDIO] Filtered Whisper hallucination on low RMS ({rms:.1f}): {text!r}")
        return ""
    return text

async def ask_llm(user_text: str, ws) -> str:
    global rate_limit_info
    settings = get_settings()
    history = get_history(limit=10)

    messages = [{"role": "system", "content": f"Your name is {settings['name']}. {settings['prompt']}"}]
    for h in history:
        messages.append({"role": h["role"], "content": h["content"]})
    messages.append({"role": "user", "content": user_text})

    resp = groq_client.chat.completions.create(
        model=settings["model"],
        messages=messages,
        tools=TOOLS,
        tool_choice="auto",
        max_tokens=800,
        temperature=0.7
    )

    # Capture rate limit headers if available
    raw_headers = getattr(getattr(resp, '_raw_response', None), 'headers', None)
    if raw_headers:
        def _h(key): return raw_headers.get(key, None)
        rate_limit_info = {
            "req_limit":       _h("x-ratelimit-limit-requests"),
            "req_remaining":   _h("x-ratelimit-remaining-requests"),
            "req_reset":       _h("x-ratelimit-reset-requests"),
            "tok_limit":       _h("x-ratelimit-limit-tokens"),
            "tok_remaining":   _h("x-ratelimit-remaining-tokens"),
            "tok_reset":       _h("x-ratelimit-reset-tokens"),
            "model":           settings["model"],
        }

    msg = resp.choices[0].message
    if msg.tool_calls:
        for tc in msg.tool_calls:
            if tc.function.name == "control_led":
                args = json.loads(tc.function.arguments)
                state = args.get("state", "off")
                await ws.send_text(json.dumps({"type": "iot", "action": "led", "state": state}))
                return f"Light {state} kar di hai."

    content = msg.content
    if not content and hasattr(msg, "reasoning") and msg.reasoning:
        content = msg.reasoning
    return (content or "Maaf kijiye, samajh nahi aaya.").strip()

async def text_to_pcm(text: str, voice: str) -> bytes:
    text = clean_for_tts(text)
    communicate = edge_tts.Communicate(text, voice)
    mp3_data = bytearray()
    async for chunk in communicate.stream():
        if chunk["type"] == "audio":
            mp3_data.extend(chunk["data"])

    proc = subprocess.Popen(
        [
            FFMPEG_PATH, "-i", "pipe:0",
            "-f", "s16le", "-acodec", "pcm_s16le",
            "-ar", "16000", "-ac", "1", "pipe:1"
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL
    )
    pcm_out, _ = proc.communicate(input=bytes(mp3_data))
    return pcm_out

# ---------------- WEBSOCKET FOR ESP32 ----------------
@app.websocket("/ws")
async def esp32_ws(ws: WebSocket):
    global active_esp32
    await ws.accept()
    active_esp32 = ws
    print("[ESP32] Connected!")
    audio_buffer = bytearray()

    try:
        while True:
            data = await ws.receive()
            if "bytes" in data and data["bytes"] is not None:
                audio_buffer.extend(data["bytes"])
            elif "text" in data and data["text"] is not None:
                msg = json.loads(data["text"])
                if msg.get("type") == "start_audio":
                    audio_buffer.clear()
                    print("[ESP32] Listening...")
                elif msg.get("type") == "end_audio":
                    if len(audio_buffer) < 4000:
                        await ws.send_text(json.dumps({"type": "status", "text": "Ready"}))
                        continue

                    await ws.send_text(json.dumps({"type": "status", "text": "Thinking..."}))
                    try:
                        user_text = await asyncio.to_thread(transcribe_audio, bytes(audio_buffer))
                        if not user_text:
                            await ws.send_text(json.dumps({"type": "status", "text": "Ready"}))
                            continue

                        add_message("user", user_text)
                        reply = await ask_llm(user_text, ws)
                        add_message("assistant", reply)

                        settings = get_settings()
                        pcm_audio = await text_to_pcm(reply, settings["voice"])

                        await ws.send_text(json.dumps({"type": "audio_start", "text": reply[:40]}))
                        chunk_size = 1024
                        for i in range(0, len(pcm_audio), chunk_size):
                            await ws.send_bytes(pcm_audio[i:i + chunk_size])
                            await asyncio.sleep(0.015)
                        await ws.send_text(json.dumps({"type": "audio_end"}))
                    except Exception as e:
                        print("[ERROR]:", e)
                        await ws.send_text(json.dumps({"type": "status", "text": "Error!"}))
    except WebSocketDisconnect:
        print("[ESP32] Disconnected")
        if active_esp32 == ws:
            active_esp32 = None

# ---------------- WEB DASHBOARD & TEXT TEST API ----------------
@app.get("/api/state")
async def api_state():
    return {
        "connected": active_esp32 is not None,
        "settings": get_settings(),
        "history": get_history(20),
        "rate_limit": rate_limit_info
    }

@app.get("/api/limits")
async def api_limits():
    """Return latest Groq rate limit info captured from last API call."""
    return rate_limit_info or {"note": "Pehle ek message send karo, phir limit dikhegi."}

@app.post("/api/test-chat")
async def api_test_chat(req: Request):
    body = await req.json()
    user_text = body.get("text", "").strip()
    if not user_text:
        return {"error": "Kuch likho pehle!"}

    iot_log = []

    class DummyWS:
        async def send_text(self, msg):
            print("[SIMULATED ESP32]:", msg)
            iot_log.append(msg)

    try:
        add_message("user", user_text)
        ws_target = active_esp32 if active_esp32 else DummyWS()
        reply = await ask_llm(user_text, ws_target)
        add_message("assistant", reply)
        return {"reply": reply, "iot": iot_log}
    except Exception as e:
        print("[TEST ERROR]:", e)
        return {"error": str(e)}

@app.post("/api/settings")
async def api_save_settings(req: Request):
    body = await req.json()
    save_settings(body)
    return {"ok": True}

@app.post("/api/clear")
async def api_clear():
    clear_history()
    return {"ok": True}

@app.post("/api/led/{state}")
async def api_led(state: str):
    if active_esp32:
        await active_esp32.send_text(json.dumps({"type": "iot", "action": "led", "state": state}))
        return {"ok": True}
    return {"ok": False, "error": "ESP32 not connected"}

@app.get("/api/speak")
async def api_speak(text: str):
    """Convert text to MP3 audio and stream to browser for playback."""
    settings = get_settings()
    voice = settings.get("voice", "hi-IN-SwaraNeural")
    clean_text = clean_for_tts(text)
    communicate = edge_tts.Communicate(clean_text, voice)
    mp3_data = bytearray()
    async for chunk in communicate.stream():
        if chunk["type"] == "audio":
            mp3_data.extend(chunk["data"])
    return StreamingResponse(
        io.BytesIO(bytes(mp3_data)),
        media_type="audio/mpeg",
        headers={"Content-Disposition": "inline"}
    )

@app.get("/", response_class=HTMLResponse)
async def dashboard():
    return """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Jarvis AI — Console</title>
<style>
  :root {
    --bg:      #080808;
    --surface: #111111;
    --panel:   #181818;
    --border:  #242424;
    --red:     #D41514;
    --silver:  #CDCBCA;
    --dim:     #555555;
    --text:    #e8e8e8;
    --green:   #22c55e;
    --blue:    #38bdf8;
    --yellow:  #fbbf24;
  }
  *{box-sizing:border-box;margin:0;padding:0}
  html,body{height:100%;font-family:'Segoe UI',system-ui,sans-serif;background:var(--bg);color:var(--text)}
  .shell{display:flex;height:100vh;overflow:hidden}
  .sidebar{width:220px;background:var(--surface);border-right:1px solid var(--border);display:flex;flex-direction:column;flex-shrink:0}
  .main{flex:1;display:flex;flex-direction:column;overflow:hidden}
  .topbar{height:54px;background:var(--surface);border-bottom:1px solid var(--border);display:flex;align-items:center;padding:0 24px;gap:12px;flex-shrink:0}
  .content{flex:1;overflow-y:auto;padding:24px}
  .page{display:none}
  .page.active{display:block}
  .logo{padding:20px 18px 14px;border-bottom:1px solid var(--border)}
  .logo-title{font-size:18px;font-weight:700;color:var(--red);letter-spacing:2px}
  .logo-sub{font-size:10px;color:var(--dim);letter-spacing:3px;margin-top:2px;text-transform:uppercase}
  .nav{flex:1;padding:10px 0}
  .nav-item{display:flex;align-items:center;gap:10px;padding:11px 18px;cursor:pointer;font-size:13px;color:var(--dim);transition:all .15s;border-left:3px solid transparent}
  .nav-item:hover{color:var(--text);background:var(--panel)}
  .nav-item.active{color:var(--red);border-left-color:var(--red);background:var(--panel);font-weight:600}
  .nav-icon{font-size:16px;width:20px;text-align:center}
  .sidebar-footer{padding:14px 18px;border-top:1px solid var(--border)}
  .topbar-title{font-size:15px;font-weight:600;flex:1}
  .badge{display:inline-flex;align-items:center;gap:5px;padding:4px 10px;border-radius:999px;font-size:11px;font-weight:700;letter-spacing:.5px}
  .badge::before{content:'';width:7px;height:7px;border-radius:50%;background:currentColor}
  .badge.online{background:#052e16;color:var(--green)}
  .badge.offline{background:#1c0a0a;color:var(--red)}
  .card{background:var(--panel);border:1px solid var(--border);border-radius:12px;padding:20px;margin-bottom:18px}
  .card-title{font-size:12px;font-weight:700;color:var(--dim);text-transform:uppercase;letter-spacing:1.5px;margin-bottom:14px}
  .chat-messages{min-height:260px;max-height:420px;overflow-y:auto;background:var(--surface);border-radius:10px;padding:14px;margin-bottom:14px;border:1px solid var(--border)}
  .msg-row{margin-bottom:14px}
  .msg-role{font-size:10px;font-weight:700;letter-spacing:1px;text-transform:uppercase;margin-bottom:4px}
  .msg-role.user{color:var(--blue)}
  .msg-role.assistant{color:var(--red)}
  .msg-bubble{background:var(--panel);border-radius:8px;padding:10px 14px;font-size:14px;line-height:1.6;border:1px solid var(--border)}
  .msg-bubble.typing{color:var(--dim);font-style:italic}
  .input-row{display:flex;gap:10px;align-items:center}
  .input-row input{flex:1;background:var(--surface);border:1px solid var(--border);color:var(--text);border-radius:8px;padding:11px 14px;font-size:14px;outline:none;transition:border .15s}
  .input-row input:focus{border-color:var(--red)}
  .input-row input::placeholder{color:var(--dim)}
  .btn{display:inline-flex;align-items:center;gap:6px;padding:10px 18px;border:none;border-radius:8px;font-size:13px;font-weight:600;cursor:pointer;transition:opacity .15s;letter-spacing:.3px}
  .btn:hover{opacity:.85}
  .btn-primary{background:var(--red);color:#fff}
  .btn-ghost{background:var(--border);color:var(--silver)}
  .btn-green{background:#052e16;color:var(--green);border:1px solid var(--green)}
  .btn-red{background:#1c0a0a;color:var(--red);border:1px solid var(--red)}
  .btn-purple{background:#2d1b69;color:#a78bfa}
  .btn-sm{padding:6px 12px;font-size:12px}
  .form-group{margin-bottom:16px}
  .form-label{font-size:11px;color:var(--dim);font-weight:600;letter-spacing:1px;text-transform:uppercase;display:block;margin-bottom:6px}
  .form-input,.form-select,.form-textarea{width:100%;background:var(--surface);border:1px solid var(--border);color:var(--text);border-radius:8px;padding:10px 14px;font-size:13px;outline:none;transition:border .15s}
  .form-input:focus,.form-select:focus,.form-textarea:focus{border-color:var(--red)}
  .form-textarea{resize:vertical;min-height:90px}
  .form-select option{background:var(--surface)}
  .btn-row{display:flex;gap:10px;flex-wrap:wrap;margin-top:4px}
  .stat-grid{display:grid;grid-template-columns:1fr 1fr;gap:14px}
  .stat-card{background:var(--surface);border:1px solid var(--border);border-radius:10px;padding:16px}
  .stat-label{font-size:10px;color:var(--dim);text-transform:uppercase;letter-spacing:1px}
  .stat-val{font-size:32px;font-weight:800;margin:6px 0 2px}
  .stat-sub{font-size:11px;color:var(--dim)}
  .bar-track{height:5px;background:var(--border);border-radius:3px;margin-top:10px;overflow:hidden}
  .bar-fill{height:100%;border-radius:3px;transition:width .6s,background .3s}
  .hist-item{padding:10px 14px;background:var(--surface);border-radius:8px;margin-bottom:8px;border:1px solid var(--border);font-size:13px;line-height:1.5}
  .hist-role{font-size:10px;font-weight:700;letter-spacing:1px;text-transform:uppercase;margin-bottom:3px}
  .hist-role.user{color:var(--blue)}
  .hist-role.assistant{color:var(--red)}
  .hist-time{font-size:10px;color:var(--dim);float:right}
  .iot-row{display:flex;align-items:center;justify-content:space-between;background:var(--surface);border:1px solid var(--border);border-radius:10px;padding:14px 18px;margin-bottom:12px}
  .voice-bar{display:flex;align-items:center;gap:10px;padding:8px 0}
  ::-webkit-scrollbar{width:5px}
  ::-webkit-scrollbar-track{background:transparent}
  ::-webkit-scrollbar-thumb{background:var(--border);border-radius:3px}
  @media(max-width:600px){
    .sidebar{width:56px}
    .nav-label,.logo-sub,.logo-title,.sidebar-footer{display:none}
    .nav-item{justify-content:center;padding:12px}
    .stat-grid{grid-template-columns:1fr}
  }
</style>
</head>
<body>
<div class="shell">
  <aside class="sidebar">
    <div class="logo">
      <div class="logo-title">JARVIS</div>
      <div class="logo-sub">AI Console v2</div>
    </div>
    <nav class="nav">
      <div class="nav-item active" onclick="showPage('chat',this)"><span class="nav-icon">💬</span><span class="nav-label">Chat</span></div>
      <div class="nav-item" onclick="showPage('settings',this)"><span class="nav-icon">⚙️</span><span class="nav-label">Settings</span></div>
      <div class="nav-item" onclick="showPage('history',this)"><span class="nav-icon">📜</span><span class="nav-label">History</span></div>
      <div class="nav-item" onclick="showPage('iot',this)"><span class="nav-icon">💡</span><span class="nav-label">IoT / LED</span></div>
      <div class="nav-item" onclick="showPage('quota',this)"><span class="nav-icon">📊</span><span class="nav-label">API Quota</span></div>
    </nav>
    <div class="sidebar-footer" style="font-size:11px;color:var(--dim)">ESP32 &nbsp;<span id="espDot">●</span></div>
  </aside>
  <div class="main">
    <div class="topbar">
      <span class="topbar-title" id="pageTitle">Chat</span>
      <span id="espBadge" class="badge offline">ESP32 Offline</span>
    </div>
    <div class="content">

      <!-- CHAT -->
      <div class="page active" id="page-chat">
        <div class="card">
          <div class="card-title">AI Response</div>
          <div class="chat-messages" id="chatBox">
            <div class="msg-row">
              <div class="msg-role assistant">Jarvis</div>
              <div class="msg-bubble typing">Yahan Jarvis ka jawab aayega...</div>
            </div>
          </div>
          <div class="voice-bar">
            <button class="btn btn-purple btn-sm" id="speakBtn" onclick="toggleSpeak()" style="display:none">🔊 Play</button>
            <label style="display:flex;align-items:center;gap:6px;cursor:pointer;font-size:12px;color:var(--dim)">
              <input type="checkbox" id="autoSpeak" checked style="accent-color:var(--red)"> Auto Voice
            </label>
          </div>
          <div class="input-row">
            <input id="testInput" placeholder="Jarvis se kuch bhi pucho..." onkeydown="if(event.key==='Enter') sendTest()">
            <button class="btn btn-primary" onclick="sendTest()">Send ➤</button>
          </div>
        </div>
      </div>

      <!-- SETTINGS -->
      <div class="page" id="page-settings">
        <div class="card">
          <div class="card-title">Assistant Identity</div>
          <div class="form-group">
            <label class="form-label">Assistant Name</label>
            <input class="form-input" id="name" placeholder="Jarvis">
          </div>
          <div class="form-group">
            <label class="form-label">Personality / System Prompt</label>
            <textarea class="form-textarea" id="prompt" rows="5"></textarea>
          </div>
        </div>
        <div class="card">
          <div class="card-title">Voice Engine</div>
          <div class="form-group">
            <label class="form-label">TTS Voice (Edge-TTS)</label>
            <select class="form-select" id="voice">
              <option value="hi-IN-SwaraNeural">🇮🇳 Hindi Female — Swara</option>
              <option value="hi-IN-MadhurNeural">🇮🇳 Hindi Male — Madhur</option>
              <option value="en-IN-NeerjaNeural">🇮🇳 English Female — Neerja</option>
              <option value="en-IN-PrabhatNeural">🇮🇳 English Male — Prabhat</option>
            </select>
          </div>
          <div class="btn-row">
            <button class="btn btn-primary" onclick="saveSettings()">💾 Save Changes</button>
          </div>
          <div id="saveMsg" style="margin-top:10px;font-size:12px;color:var(--green);display:none">✓ Settings saved!</div>
        </div>
      </div>

      <!-- HISTORY -->
      <div class="page" id="page-history">
        <div class="card">
          <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:14px">
            <span class="card-title" style="margin-bottom:0">Conversation Memory</span>
            <button class="btn btn-red btn-sm" onclick="clearMem()">🗑 Clear All</button>
          </div>
          <div id="historyList"><div style="color:var(--dim);font-size:13px">No history yet.</div></div>
        </div>
      </div>

      <!-- IOT -->
      <div class="page" id="page-iot">
        <div class="card">
          <div class="card-title">IoT Device Control</div>
          <div class="iot-row">
            <div>
              <div style="font-size:14px;font-weight:600">💡 Room LED / Light</div>
              <div style="font-size:11px;color:var(--dim);margin-top:2px">ESP32 GPIO controlled</div>
            </div>
            <div class="btn-row">
              <button class="btn btn-green btn-sm" onclick="ledCtrl('on')">ON</button>
              <button class="btn btn-red btn-sm" onclick="ledCtrl('off')">OFF</button>
            </div>
          </div>
        </div>
        <div class="card">
          <div class="card-title">Voice Command Examples</div>
          <div style="font-size:13px;color:var(--dim);line-height:2.2">
            🎙 <b style="color:var(--text)">"Light on karo"</b><br>
            🎙 <b style="color:var(--text)">"Batti bujha do"</b><br>
            🎙 <b style="color:var(--text)">"Turn off the light"</b><br>
            🎙 <b style="color:var(--text)">"Kamra roshan karo"</b>
          </div>
        </div>
        <div class="card">
          <div class="card-title">ESP32 Connection</div>
          <div id="esp32Status" style="font-size:24px;font-weight:800;color:var(--dim)">—</div>
          <div style="font-size:11px;color:var(--dim);margin-top:4px">WebSocket status</div>
        </div>
      </div>

      <!-- QUOTA -->
      <div class="page" id="page-quota">
        <div class="card">
          <div class="card-title">Groq API Usage</div>
          <div class="stat-grid">
            <div class="stat-card">
              <div class="stat-label">Requests Remaining</div>
              <div class="stat-val" id="reqRem" style="color:var(--green)">—</div>
              <div class="stat-sub" id="reqLim">of — | Reset: —</div>
              <div class="bar-track"><div class="bar-fill" id="reqBar" style="width:0%;background:var(--green)"></div></div>
            </div>
            <div class="stat-card">
              <div class="stat-label">Tokens Remaining</div>
              <div class="stat-val" id="tokRem" style="color:var(--blue)">—</div>
              <div class="stat-sub" id="tokLim">of — | Reset: —</div>
              <div class="bar-track"><div class="bar-fill" id="tokBar" style="width:0%;background:var(--blue)"></div></div>
            </div>
          </div>
          <div style="margin-top:14px;font-size:12px;color:var(--dim)" id="quotaNote">Send a message first to see quota data.</div>
        </div>
        <div class="card">
          <div class="card-title">Active Model</div>
          <div id="modelName" style="font-size:18px;font-weight:700;color:var(--silver)">—</div>
        </div>
      </div>

    </div>
  </div>
</div>
<script>
  function showPage(id,navEl){
    document.querySelectorAll('.page').forEach(p=>p.classList.remove('active'));
    document.getElementById('page-'+id).classList.add('active');
    document.querySelectorAll('.nav-item').forEach(n=>n.classList.remove('active'));
    if(navEl)navEl.classList.add('active');
    const t={chat:'Chat',settings:'Settings',history:'History',iot:'IoT / LED',quota:'API Quota'};
    document.getElementById('pageTitle').textContent=t[id]||id;
    if(id==='history')load();
  }
  let currentAudio=null,audioStopped=false;
  function stopAudio(){
    audioStopped=true;
    if(currentAudio){currentAudio.onended=null;currentAudio.onerror=null;currentAudio.pause();currentAudio.src='';currentAudio.load();currentAudio=null;}
    const b=document.getElementById('speakBtn');
    if(b){b.textContent='🔊 Play';b.style.background='#2d1b69';}
  }
  function speakText(text){
    stopAudio();audioStopped=false;
    const url='/api/speak?text='+encodeURIComponent(text);
    currentAudio=new Audio();currentAudio.preload='auto';
    currentAudio.oncanplay=function(){
      if(audioStopped)return;
      this.play().catch(()=>{});
      const b=document.getElementById('speakBtn');
      if(b){b.textContent='⏹ Stop';b.style.background='#7f1d1d';}
    };
    currentAudio.onended=function(){
      const b=document.getElementById('speakBtn');
      if(b){b.textContent='🔊 Play';b.style.background='#2d1b69';}
      currentAudio=null;
    };
    currentAudio.onerror=function(){currentAudio=null;};
    currentAudio.src=url;
  }
  function toggleSpeak(){
    if(currentAudio&&!currentAudio.paused)stopAudio();
    else if(window._lastReply)speakText(window._lastReply);
  }
  function escH(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}
  async function sendTest(){
    const inp=document.getElementById('testInput');
    const box=document.getElementById('chatBox');
    const txt=inp.value.trim();if(!txt)return;
    box.innerHTML+=`<div class="msg-row"><div class="msg-role user">You</div><div class="msg-bubble">${escH(txt)}</div></div>`;
    const tid='t'+Date.now();
    box.innerHTML+=`<div class="msg-row" id="${tid}"><div class="msg-role assistant">Jarvis</div><div class="msg-bubble typing">Thinking...</div></div>`;
    box.scrollTop=box.scrollHeight;
    document.getElementById('speakBtn').style.display='none';
    inp.value='';
    const res=await fetch('/api/test-chat',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({text:txt})}).then(x=>x.json());
    const te=document.getElementById(tid);if(te)te.remove();
    if(res.error){
      box.innerHTML+=`<div class="msg-row"><div class="msg-role assistant">Jarvis</div><div class="msg-bubble" style="color:var(--red)">Error: ${escH(res.error)}</div></div>`;
    }else{
      let iotTag='';
      if(res.iot&&res.iot.length>0){try{const c=JSON.parse(res.iot[0]);iotTag=`<span style="font-size:11px;color:var(--green);margin-left:8px">💡 Light ${c.state.toUpperCase()}</span>`;}catch(e){}}
      box.innerHTML+=`<div class="msg-row"><div class="msg-role assistant">Jarvis ${iotTag}</div><div class="msg-bubble">${escH(res.reply)}</div></div>`;
      window._lastReply=res.reply;
      document.getElementById('speakBtn').style.display='inline-flex';
      if(document.getElementById('autoSpeak').checked)speakText(res.reply);
    }
    box.scrollTop=box.scrollHeight;
    load();
  }
  async function saveSettings(){
    await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:document.getElementById('name').value,prompt:document.getElementById('prompt').value,voice:document.getElementById('voice').value})});
    const m=document.getElementById('saveMsg');m.style.display='block';setTimeout(()=>m.style.display='none',2500);
  }
  async function ledCtrl(s){await fetch('/api/led/'+s,{method:'POST'});}
  async function clearMem(){await fetch('/api/clear',{method:'POST'});load();}
  async function load(){
    const r=await fetch('/api/state').then(x=>x.json());
    const conn=r.connected;
    const b=document.getElementById('espBadge');
    b.textContent=conn?'ESP32 Online':'ESP32 Offline';b.className='badge '+(conn?'online':'offline');
    document.getElementById('espDot').style.color=conn?'var(--green)':'var(--red)';
    const es=document.getElementById('esp32Status');
    if(es){es.textContent=conn?'🟢 Connected':'🔴 Disconnected';es.style.color=conn?'var(--green)':'var(--red)';}
    if(!window.loadedOnce){
      document.getElementById('name').value=r.settings.name;
      document.getElementById('prompt').value=r.settings.prompt;
      document.getElementById('voice').value=r.settings.voice;
      window.loadedOnce=true;
    }
    const hl=document.getElementById('historyList');
    if(hl){
      if(r.history&&r.history.length>0){
        hl.innerHTML=r.history.map(m=>`<div class="hist-item"><div class="hist-role ${m.role}">${m.role.toUpperCase()} <span class="hist-time">${m.timestamp}</span></div><div>${escH(m.content)}</div></div>`).join('');
      }else{hl.innerHTML='<div style="color:var(--dim);font-size:13px">No history yet.</div>';}
    }
    const rl=r.rate_limit||{};
    if(rl.req_remaining!=null){
      const rr=parseInt(rl.req_remaining)||0,rl2=parseInt(rl.req_limit)||1;
      const tr=parseInt(rl.tok_remaining)||0,tl=parseInt(rl.tok_limit)||1;
      const rP=Math.round(rr/rl2*100),tP=Math.round(tr/tl*100);
      const rC=rP>50?'var(--green)':rP>20?'var(--yellow)':'var(--red)';
      const tC=tP>50?'var(--blue)':tP>20?'var(--yellow)':'var(--red)';
      document.getElementById('reqRem').textContent=rr.toLocaleString();document.getElementById('reqRem').style.color=rC;
      document.getElementById('reqLim').textContent=`of ${rl2.toLocaleString()} | Reset: ${rl.req_reset||'—'}`;
      document.getElementById('reqBar').style.cssText=`width:${rP}%;background:${rC}`;
      document.getElementById('tokRem').textContent=tr.toLocaleString();document.getElementById('tokRem').style.color=tC;
      document.getElementById('tokLim').textContent=`of ${tl.toLocaleString()} | Reset: ${rl.tok_reset||'—'}`;
      document.getElementById('tokBar').style.cssText=`width:${tP}%;background:${tC}`;
      document.getElementById('quotaNote').textContent='';
      const mn=document.getElementById('modelName');if(mn)mn.textContent=rl.model||'—';
    }
  }
  load();setInterval(load,4000);
</script>
</body>
</html>"""
