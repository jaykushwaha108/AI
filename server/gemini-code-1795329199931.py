from __future__ import annotations
import os
import io
import re
import json
import wave
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

def transcribe_audio(pcm_bytes: bytes) -> str:
    wav_bytes = pcm_to_wav(pcm_bytes)
    resp = groq_client.audio.transcriptions.create(
        file=("audio.wav", wav_bytes),
        model="whisper-large-v3-turbo",
        response_format="text"
    )
    return resp.strip()

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
    return """
    <!DOCTYPE html>
    <html>
    <head>
      <title>Custom AI Console</title>
      <meta name="viewport" content="width=device-width, initial-scale=1">
      <style>
        body { font-family: system-ui, sans-serif; background: #0f172a; color: #e2e8f0; max-width: 800px; margin: auto; padding: 20px; }
        .card { background: #1e293b; padding: 18px; border-radius: 12px; margin-bottom: 16px; }
        input, textarea, select { width: 100%; padding: 10px; margin: 6px 0 12px; background: #0f172a; color: #fff; border: 1px solid #334155; border-radius: 8px; box-sizing: border-box; }
        button { background: #3b82f6; color: #fff; border: none; padding: 10px 16px; border-radius: 8px; cursor: pointer; margin-right: 8px; }
        button.danger { background: #ef4444; }
        button.green { background: #10b981; }
        .badge { padding: 4px 10px; border-radius: 999px; font-size: 13px; font-weight: bold; }
        .on { background: #065f46; color: #34d399; }
        .off { background: #7f1d1d; color: #f87171; }
        .msg { padding: 8px 12px; margin: 6px 0; border-radius: 8px; background: #0f172a; }
        .reply-box { margin-top: 12px; padding: 10px; border-radius: 8px; background: #0f172a; color: #38bdf8; min-height: 20px; }
      </style>
    </head>
    <body>
      <h2>My AI Assistant Console <span id="status" class="badge off">Offline</span></h2>

      <div class="card">
        <h3>🤖 Chat with Jarvis</h3>
        <div style="display:flex;gap:8px;align-items:center">
          <input id="testInput" placeholder="Kuch bhi pucho... (Enter dabao ya Send karo)" onkeydown="if(event.key==='Enter') sendTest()" style="margin:0">
          <button class="green" onclick="sendTest()" style="white-space:nowrap">Send 🚀</button>
        </div>
        <div id="testReply" class="reply-box" style="font-size:16px;line-height:1.6">Answer yahan dikhega...</div>
        <div style="margin-top:8px;display:flex;align-items:center;gap:10px">
          <button id="speakBtn" onclick="toggleSpeak()" style="display:none;background:#7c3aed">🔊 Dobara Suno</button>
          <label style="display:flex;align-items:center;gap:6px;cursor:pointer;font-size:14px">
            <input type="checkbox" id="autoSpeak" checked style="width:auto;margin:0"> Auto Voice
          </label>
        </div>
      </div>

      <div class="card">
        <h3>Role & Voice Settings</h3>
        <label>Assistant Name</label><input id="name">
        <label>System Prompt / Personality</label><textarea id="prompt" rows="3"></textarea>
        <label>Voice (Free Edge-TTS)</label>
        <select id="voice">
          <option value="hi-IN-SwaraNeural">Hindi Female (Swara)</option>
          <option value="hi-IN-MadhurNeural">Hindi Male (Madhur)</option>
          <option value="en-IN-NeerjaNeural">Indian English Female (Neerja)</option>
          <option value="en-IN-PrabhatNeural">Indian English Male (Prabhat)</option>
        </select>
        <button onclick="save()">Save Settings</button>
        <button class="green" onclick="led('on')">LED ON</button>
        <button class="danger" onclick="led('off')">LED OFF</button>
      </div>

      <div class="card">
        <h3>Conversation Memory <button class="danger" style="float:right;padding:4px 10px" onclick="clearMem()">Clear</button></h3>
        <div id="logs"></div>
      </div>

      <div class="card" id="quotaCard">
        <h3>📊 Groq API Quota <small style="font-size:12px;color:#64748b">(last request ke baad update hota hai)</small></h3>
        <div id="quotaBox" style="display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:10px">
          <div style="background:#0f172a;border-radius:8px;padding:12px">
            <div style="font-size:11px;color:#64748b;text-transform:uppercase;letter-spacing:1px">Requests Remaining</div>
            <div id="reqRem" style="font-size:28px;font-weight:bold;color:#34d399">—</div>
            <div id="reqLim" style="font-size:12px;color:#64748b">of — | Reset: —</div>
            <div style="margin-top:6px;background:#1e293b;border-radius:4px;height:6px;overflow:hidden">
              <div id="reqBar" style="height:100%;background:#34d399;width:0%;transition:width 0.5s"></div>
            </div>
          </div>
          <div style="background:#0f172a;border-radius:8px;padding:12px">
            <div style="font-size:11px;color:#64748b;text-transform:uppercase;letter-spacing:1px">Tokens Remaining</div>
            <div id="tokRem" style="font-size:28px;font-weight:bold;color:#38bdf8">—</div>
            <div id="tokLim" style="font-size:12px;color:#64748b">of — | Reset: —</div>
            <div style="margin-top:6px;background:#1e293b;border-radius:4px;height:6px;overflow:hidden">
              <div id="tokBar" style="height:100%;background:#38bdf8;width:0%;transition:width 0.5s"></div>
            </div>
          </div>
        </div>
        <div id="quotaNote" style="margin-top:8px;font-size:12px;color:#64748b">Model: —</div>
      </div>

      <script>
        async function load() {
          const r = await fetch('/api/state').then(x => x.json());
          const st = document.getElementById('status');
          st.textContent = r.connected ? 'ESP32 Online' : 'ESP32 Offline';
          st.className = 'badge ' + (r.connected ? 'on' : 'off');
          if (!window.loadedOnce) {
            document.getElementById('name').value = r.settings.name;
            document.getElementById('prompt').value = r.settings.prompt;
            document.getElementById('voice').value = r.settings.voice;
            window.loadedOnce = true;
          }
          document.getElementById('logs').innerHTML = r.history.map(m =>
            `<div class="msg"><b>${m.role.toUpperCase()}</b> <small>(${m.timestamp})</small>: ${m.content}</div>`
          ).join('');

          // Update quota card
          const rl = r.rate_limit || {};
          if (rl.req_remaining !== undefined && rl.req_remaining !== null) {
            const rrem = parseInt(rl.req_remaining) || 0;
            const rlim = parseInt(rl.req_limit) || 1;
            const trem = parseInt(rl.tok_remaining) || 0;
            const tlim = parseInt(rl.tok_limit) || 1;
            const rPct = Math.round(rrem / rlim * 100);
            const tPct = Math.round(trem / tlim * 100);
            const rColor = rPct > 50 ? '#34d399' : rPct > 20 ? '#fbbf24' : '#f87171';
            const tColor = tPct > 50 ? '#38bdf8' : tPct > 20 ? '#fbbf24' : '#f87171';
            document.getElementById('reqRem').textContent = rrem.toLocaleString();
            document.getElementById('reqRem').style.color = rColor;
            document.getElementById('reqLim').textContent = `of ${rlim.toLocaleString()} | Reset: ${rl.req_reset || '—'}`;
            document.getElementById('reqBar').style.width = rPct + '%';
            document.getElementById('reqBar').style.background = rColor;
            document.getElementById('tokRem').textContent = trem.toLocaleString();
            document.getElementById('tokRem').style.color = tColor;
            document.getElementById('tokLim').textContent = `of ${tlim.toLocaleString()} | Reset: ${rl.tok_reset || '—'}`;
            document.getElementById('tokBar').style.width = tPct + '%';
            document.getElementById('tokBar').style.background = tColor;
            document.getElementById('quotaNote').textContent = 'Model: ' + (rl.model || '—');
          } else {
            document.getElementById('quotaNote').textContent = 'Pehle ek message send karo — phir quota yahan dikhega.';
          }
        }

        async function sendTest() {
          const inp = document.getElementById('testInput');
          const box = document.getElementById('testReply');
          const txt = inp.value.trim();
          if (!txt) return;
          box.textContent = '⏳ Thinking...';
          box.style.color = '#94a3b8';
          document.getElementById('speakBtn').style.display = 'none';
          const res = await fetch('/api/test-chat', {
            method: 'POST', headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({text: txt})
          }).then(x => x.json());

          if (res.error) {
            box.style.color = '#f87171';
            box.textContent = '❌ Error: ' + res.error;
          } else {
            box.style.color = '#38bdf8';
            let out = res.reply;
            if (res.iot && res.iot.length > 0) {
              try {
                const cmd = JSON.parse(res.iot[0]);
                out += '  💡 [Light ' + (cmd.state === 'on' ? 'ON ✅' : 'OFF 🔴') + ']';
              } catch(e) {}
            }
            box.textContent = out;
            window._lastReply = res.reply;
            document.getElementById('speakBtn').style.display = 'inline-block';
            if (document.getElementById('autoSpeak').checked) {
              speakText(res.reply);   // stopAudio() internally call hoti hai
            }
          }
          inp.value = '';
          load();
        }

        // ── Audio player: proper stop/start with no ghost playback ──
        let currentAudio = null;
        let audioStopped = false;   // Flag: user ne manually stop kiya

        function stopAudio() {
          audioStopped = true;
          if (currentAudio) {
            currentAudio.onended = null;   // Remove any callbacks
            currentAudio.onerror = null;
            currentAudio.pause();
            currentAudio.src = '';         // Browser download cancel karo
            currentAudio.load();           // Reset media element
            currentAudio = null;
          }
          const btn = document.getElementById('speakBtn');
          if (btn) { btn.textContent = '🔊 Dobara Suno'; btn.style.background = '#7c3aed'; }
        }

        function speakText(text) {
          stopAudio();              // Pehle purana band karo
          audioStopped = false;    // Reset flag for new play
          const url = '/api/speak?text=' + encodeURIComponent(text);
          currentAudio = new Audio();
          currentAudio.preload = 'auto';

          currentAudio.oncanplay = function() {
            if (audioStopped) return;   // User ne stop kar diya tha
            this.play().catch(() => {});
            const btn = document.getElementById('speakBtn');
            if (btn) { btn.textContent = '⏹ Stop'; btn.style.background = '#ef4444'; }
          };
          currentAudio.onended = function() {
            const btn = document.getElementById('speakBtn');
            if (btn) { btn.textContent = '🔊 Dobara Suno'; btn.style.background = '#7c3aed'; }
            currentAudio = null;
          };
          currentAudio.onerror = function() {
            currentAudio = null;
          };
          currentAudio.src = url;  // Src set karo — load shuru hoga
        }

        function toggleSpeak() {
          if (currentAudio && !currentAudio.paused) {
            stopAudio();   // Playing hai → Stop karo
          } else {
            if (window._lastReply) speakText(window._lastReply);  // Stopped hai → Play karo
          }
        }

        function speakLast() {
          if (window._lastReply) speakText(window._lastReply);
        }

        async function save() {
          await fetch('/api/settings', {
            method: 'POST', headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({
              name: document.getElementById('name').value,
              prompt: document.getElementById('prompt').value,
              voice: document.getElementById('voice').value
            })
          });
          alert('Saved!');
        }
        async function clearMem() { await fetch('/api/clear', {method: 'POST'}); load(); }
        async function led(s) { await fetch('/api/led/' + s, {method: 'POST'}); }
        load(); setInterval(load, 3000);
      </script>
    </body>
    </html>
    """

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
