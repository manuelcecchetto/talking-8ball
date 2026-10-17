# Server: the 8-ball's brain on your PC

The ball records your question and sends it here. Whisper turns it into text on this PC, pi answers
as the 8-ball (cryptic, 2–7 words, in the language you asked in), and the answer goes back to the
ball's screen.

```
ball ──WAV over Wi-Fi──▶ server.py ──▶ Whisper (GPU) ──▶ pi (RPC, claude-bridge/claude-haiku-5-5)
     ◀──── {"answer": "..."} ──────────────────────────────────────┘
```

## Run

```bat
server\start.cmd
```

The first run creates `server\.venv` and downloads the Whisper model (~1.6 GB, once). Then open
**http://localhost:8788/**: the same reveal animation as `reveal.html`, but live with your
microphone. Press **S** (or Shake), ask out loud, and stop talking: it sends when you go quiet.
Holding **Space** is push-to-talk; you can also type a question and press Enter.

Options: `--model provider/id` (any model `pi --list-models` shows and your login supports),
`--whisper small` and `--device cpu` for a PC without an NVIDIA GPU, `--port`.

Requirements: [pi](https://pi.dev) installed and signed in (the server runs `pi --mode rpc`, so the
answers use your existing pi login, no extra API key); Python 3.12 (or `uv`).

Measured on an RTX 3090: about 0.5 s to transcribe, 1.5–2.5 s for pi to answer.

**Windows firewall:** the first time, Windows asks whether Python may accept connections. Allow
**private networks**, otherwise the ball can't reach the PC.

## Protocol (what the firmware implements)

| | |
|---|---|
| Discovery | mDNS service `_8ball._tcp`, TXT `path=/ask`. Or configure `http://<pc-ip>:8788` by hand |
| Ask | `POST /ask` with header `Authorization: Bearer <token>`, body = WAV, **16 kHz, mono, 16-bit PCM**, at most 15 s |
| Answer | `200 {"question": "...", "language": "it", "answer": "...", "source": "pi" \| "fallback" \| "no_speech", "ms": {"hear": 500, "think": 1800}}` |
| Errors | `401 {"error": "unauthorized"}` (bad token), `400 {"error": "..."}` (bad audio) |
| Debug | `POST /ask_text {"text": "...", "language": "it"}`, `GET /health` |

The token is created on the first run in `server/config.json` (not committed) and printed at
startup; the ball stores it during setup. Requests from this PC's own browser need no token; a page
from any other site is refused.

If pi fails or times out, the ball still gets an in-character answer ("Ask again later",
`source: "fallback"`). If nothing was said, `source: "no_speech"`.
