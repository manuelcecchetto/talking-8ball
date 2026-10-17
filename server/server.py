"""Talking 8-ball server: WAV in, cryptic answer out.

The ball (or the browser test page) POSTs 16 kHz mono 16-bit WAV to /ask. Whisper transcribes it on
this PC and pi answers as the 8-ball, in the language of the question.

Run:  server/.venv/Scripts/python server/server.py      then open http://localhost:8788/
"""
import argparse
import asyncio
import glob
import io
import json
import logging
import os
import re
import secrets
import shutil
import site
import socket
import sys
import time
import wave
from pathlib import Path

import numpy as np
from aiohttp import web

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CONFIG = HERE / "config.json"         # holds the device token; not committed
log = logging.getLogger("8ball")

SYSTEM_PROMPT = """You are the Talking 8-Ball: an oracle sealed in a black ball of dark blue liquid.
Someone just shook you and asked a question out loud. Reply with ONE cryptic, oracular answer of 2 to
7 words, the way a magic 8-ball would, but fitted to the question.
Like the real 8-ball, lean toward a verdict (yes, no, not yet, maybe) and wrap it in a short image
or twist drawn from the question itself. Vary your imagery; avoid stock fog and mist.
Tone: mysterious, playful, a little wry, never rude, never preachy.
Answer in the language you are told, in correct, natural spelling. Write numbers as digits (42), never
spelled out. Output only the answer: no quotes, no emoji, no explanation,
no follow-up question, no final period.
If the words are unclear or not a question, give a playful mysterious deflection.
Never mention being an AI or a model."""

# When pi can't answer, the ball still says something in character.
FALLBACK = {
    "it": "Chiedi di nuovo più tardi",
    "en": "Ask again later",
    "es": "Pregunta más tarde",
    "fr": "Redemande plus tard",
    "de": "Frag später noch einmal",
}
NO_SPEECH = {"it": "Non ti ho sentito", "en": "I did not hear you"}
LANGUAGES = {"it": "Italian", "en": "English", "es": "Spanish", "fr": "French", "de": "German",
             "pt": "Portuguese", "nl": "Dutch"}
MAX_AUDIO_SECONDS = 15
SAMPLE_RATE = 16000


# --- speech to text ------------------------------------------------------------------
def add_cuda_dlls():
    """CTranslate2 on Windows finds cuBLAS/cuDNN only if their pip wheels' bin folders are searchable."""
    for sp in site.getsitepackages():
        for d in glob.glob(os.path.join(sp, "nvidia", "*", "bin")):
            os.add_dll_directory(d)
            os.environ["PATH"] = d + os.pathsep + os.environ["PATH"]


class Ears:
    def __init__(self, model: str, device: str):
        if sys.platform == "win32":
            add_cuda_dlls()
        from faster_whisper import WhisperModel
        try:
            self.model = WhisperModel(model, device=device, compute_type="float16" if device != "cpu" else "int8")
        except Exception as e:  # no usable GPU: a smaller model on the CPU still works
            log.warning("Whisper on %s failed (%s), using 'small' on the CPU", device, e)
            self.model = WhisperModel("small", device="cpu", compute_type="int8")

    def hear(self, pcm: np.ndarray) -> tuple[str, str]:
        segments, info = self.model.transcribe(pcm, beam_size=1, vad_filter=True,
                                               condition_on_previous_text=False)
        return " ".join(s.text for s in segments).strip(), info.language


def read_wav(data: bytes) -> np.ndarray:
    """16 kHz mono PCM16 WAV -> float32 samples. Anything else is refused (the senders control the format)."""
    with wave.open(io.BytesIO(data)) as w:
        if w.getsampwidth() != 2 or w.getnchannels() != 1 or w.getframerate() != SAMPLE_RATE:
            raise ValueError(f"need 16 kHz mono 16-bit WAV, got {w.getframerate()} Hz, "
                             f"{w.getnchannels()} ch, {8 * w.getsampwidth()} bit")
        if w.getnframes() > MAX_AUDIO_SECONDS * SAMPLE_RATE:
            raise ValueError(f"audio longer than {MAX_AUDIO_SECONDS} s")
        return np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(np.float32) / 32768


# --- the oracle: one warm pi process in RPC mode ----------------------------------------
def pi_command() -> list[str]:
    """How to launch pi. On Windows `pi` is an npm .cmd shim, and cmd.exe cuts an argument at its first
    newline (the system prompt arrived as one line), so run its cli.js with node instead."""
    pi = shutil.which("pi")
    if not pi:
        raise RuntimeError("pi not found on PATH")
    if pi.lower().endswith(".cmd"):
        m = re.search(r'"%dp0%\\([^"]+\.js)"', Path(pi).read_text(errors="ignore"))
        node = shutil.which("node")
        if m and node:
            return [node, str(Path(pi).parent / m.group(1))]
    return [pi]


class Oracle:
    def __init__(self, model: str, timeout: float = 25.0):
        self.model, self.timeout = model, timeout
        self.proc: asyncio.subprocess.Process | None = None
        self.lock = asyncio.Lock()
        self.ids = 0

    async def start(self):
        self.proc = await asyncio.create_subprocess_exec(
            *pi_command(), "--mode", "rpc", "--no-session", "--no-tools", "--no-skills", "--no-context-files",
            "--no-mcp", "--no-prompt-templates", "--no-themes", "--offline", "--thinking", "off",
            "--model", self.model, "--system-prompt", SYSTEM_PROMPT,
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL,
            cwd=str(HERE), limit=1 << 20)
        log.info("pi started (%s)", self.model)

    async def stop(self):
        if self.proc and self.proc.returncode is None:
            self.proc.kill()
            await self.proc.wait()
        self.proc = None

    async def _send(self, cmd: dict) -> str:
        self.ids += 1
        cmd["id"] = f"q{self.ids}"
        self.proc.stdin.write((json.dumps(cmd) + "\n").encode())
        await self.proc.stdin.drain()
        return cmd["id"]

    async def _until(self, done) -> list[dict]:
        records = []
        while True:
            line = await self.proc.stdout.readline()
            if not line:
                raise RuntimeError("pi exited")
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            records.append(rec)
            if done(rec):
                return records

    async def _ask(self, question: str, language: str) -> str:
        if not self.proc or self.proc.returncode is not None:
            await self.start()
        rid = await self._send({"type": "new_session"})       # every question starts clean
        await self._until(lambda r: r.get("type") == "response" and r.get("id") == rid)
        lang = LANGUAGES.get(language, language)
        rid = await self._send({"type": "prompt", "message": f"Language: {lang}\nQuestion: {question}"})
        # The streamed text_delta pieces can split an accented letter in two (seen as U+FFFD), so the
        # answer comes from the final message; the deltas are only a fallback.
        text, final, errors = [], [], []
        for rec in await self._until(lambda r: r.get("type") == "agent_settled"
                                     or (r.get("type") == "response" and r.get("id") == rid
                                         and not r.get("success"))):
            if rec.get("type") == "message_update":
                ev = rec.get("assistantMessageEvent", {})
                if ev.get("type") == "text_delta":
                    text.append(ev.get("delta", ""))
            elif rec.get("type") == "response" and not rec.get("success"):
                errors.append(rec.get("error"))
            elif rec.get("type") == "message_end":
                msg = rec.get("message", {})
                if msg.get("stopReason") == "error" or msg.get("errorMessage"):
                    errors.append(msg.get("errorMessage"))
                elif msg.get("role") == "assistant":
                    final += [c.get("text", "") for c in msg.get("content", []) if c.get("type") == "text"]
        if errors and not (final or text):
            raise RuntimeError("; ".join(str(e) for e in errors))
        return "".join(final) or "".join(text)

    async def ask(self, question: str, language: str) -> str:
        async with self.lock:
            try:
                return await asyncio.wait_for(self._ask(question, language), self.timeout)
            except Exception:
                await self.stop()          # a fresh pi next time
                raise


MAX_WORDS = 9
# The claude-bridge provider wraps our prompt in its own instructions; now and then the model talks
# about those instead of answering. Such replies are refused and asked again.
META = re.compile(r"(?i:\b(system|sistema|prompt|context|contesto|instruction|istruzion\w*|model|modello|"
                  r"assistant|assistente|claude|anthropic|quarantin\w*|token)\b)"
                  r"|\b(AI|IA)\b")              # case-sensitive: Italian "ai" is a preposition


def clean(answer: str) -> str:
    line = next((l for l in answer.strip().splitlines() if l.strip()), "")
    line = re.sub(r"[*_`#]", "", line).strip().strip('"“”«»\'').strip()
    return re.sub(r"(?<!\.)\.$", "", line.rstrip(" ,;:-"))   # keep "...", drop a lone period


def acceptable(line: str) -> bool:
    return 0 < len(line.split()) <= MAX_WORDS and len(line) <= 64 and not META.search(line)


def shorten(line: str) -> str | None:
    """Last resort for a long answer: keep it up to a clause boundary, never mid-phrase."""
    words = line.split()[:MAX_WORDS]
    for i in range(len(words), 1, -1):
        if re.search(r"[,;:.!?…]$", words[i - 1]):
            return clean(" ".join(words[:i]))
    return None


# --- HTTP ---------------------------------------------------------------------------------
def load_token() -> str:
    cfg = json.loads(CONFIG.read_text()) if CONFIG.exists() else {}
    if not cfg.get("token"):
        cfg["token"] = secrets.token_urlsafe(18)
        CONFIG.write_text(json.dumps(cfg, indent=2))
    return cfg["token"]


def allowed(request: web.Request) -> bool:
    """The ball sends the token. The test page on this PC needs none, but browsers on other sites
    must not be able to drive it (Origin must be ours when present)."""
    token = request.app["token"]
    if secrets.compare_digest(request.headers.get("Authorization", ""), f"Bearer {token}"):
        return True
    peer = request.remote or ""
    origin = request.headers.get("Origin")
    local = peer in ("127.0.0.1", "::1")
    return local and (origin is None or origin == f"http://{request.host}")


async def answer(app, question: str, language: str) -> tuple[str, str]:
    fallback = FALLBACK.get(language, FALLBACK["en"])
    nudge = ""
    try:
        for attempt in range(2):
            raw = await app["oracle"].ask(question + nudge, language)
            line = clean(raw)
            if acceptable(line):
                return line, "pi"
            log.info("refused answer %r (attempt %d)", raw, attempt + 1)
            nudge = ("\n(Your last reply was too long or not in character. Answer as the 8-ball only, "
                     "at most 6 words.)")
        short = shorten(line)
        return (short, "pi") if short and not META.search(short) else (fallback, "fallback")
    except Exception as e:
        log.warning("pi failed: %s", e)
        return fallback, "fallback"


async def ask(request: web.Request):
    if not allowed(request):
        return web.json_response({"error": "unauthorized"}, status=401)
    data = await request.read()
    t0 = time.perf_counter()
    try:
        pcm = read_wav(data)
    except (ValueError, wave.Error, EOFError) as e:
        return web.json_response({"error": str(e) or "not a WAV file"}, status=400)
    question, language = await asyncio.to_thread(request.app["ears"].hear, pcm)
    t1 = time.perf_counter()
    seconds = len(pcm) / SAMPLE_RATE
    if len(question) < 2:
        # Whisper's language guess on silence is noise: answer in the language last spoken.
        language = request.app["last_language"]
        result = {"question": "", "language": language, "answer": NO_SPEECH.get(language, NO_SPEECH["en"]),
                  "source": "no_speech"}
    else:
        request.app["last_language"] = language
        reply, source = await answer(request.app, question, language)
        result = {"question": question, "language": language, "answer": reply, "source": source}
    t2 = time.perf_counter()
    result["ms"] = {"hear": round(1000 * (t1 - t0)), "think": round(1000 * (t2 - t1))}
    log.info("%s | %.1f s audio | %r -> %r (%s ms)", language, seconds, question, result["answer"], result["ms"])
    return web.json_response(result)


async def ask_text(request: web.Request):
    """Debug: skip the microphone. POST {"text": "...", "language": "it"}."""
    if not allowed(request):
        return web.json_response({"error": "unauthorized"}, status=401)
    try:
        body = json.loads(await request.text())
    except (json.JSONDecodeError, UnicodeDecodeError):
        return web.json_response({"error": "body must be UTF-8 JSON"}, status=400)
    t0 = time.perf_counter()
    reply, source = await answer(request.app, str(body.get("text", ""))[:300], str(body.get("language", "en")))
    return web.json_response({"answer": reply, "source": source, "ms": {"think": round(1000 * (time.perf_counter() - t0))}})


async def index(request: web.Request):
    if (request.remote or "") not in ("127.0.0.1", "::1"):
        raise web.HTTPForbidden()
    return web.FileResponse(ROOT / "reveal.html")


async def health(request: web.Request):
    return web.json_response({"ok": True, "name": "talking-8ball"})


# --- discovery: the ball finds this PC as _8ball._tcp on the local network -----------------
def lan_ip() -> str:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.connect(("10.255.255.255", 1))
            return s.getsockname()[0]
        except OSError:
            return "127.0.0.1"


async def advertise(app):
    try:
        from zeroconf import ServiceInfo
        from zeroconf.asyncio import AsyncZeroconf
    except ImportError:
        log.info("zeroconf not installed: the ball needs the server address configured by hand")
        yield
        return
    ip = lan_ip()
    info = ServiceInfo("_8ball._tcp.local.", "talking-8ball._8ball._tcp.local.",
                       addresses=[socket.inet_aton(ip)], port=app["port"], properties={"path": "/ask"})
    zc = AsyncZeroconf()
    await zc.async_register_service(info)
    log.info("advertised as _8ball._tcp on %s:%d", ip, app["port"])
    yield
    await zc.async_unregister_service(info)
    await zc.async_close()


async def lifecycle(app):
    await app["oracle"].start()
    yield
    await app["oracle"].stop()


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--host", default="0.0.0.0", help="0.0.0.0 lets the ball reach it over Wi-Fi")
    p.add_argument("--port", type=int, default=8788)
    p.add_argument("--model", default="claude-bridge/claude-sonnet-5-5", help="pi model (provider/id)")
    p.add_argument("--whisper", default="large-v3-turbo")
    p.add_argument("--device", default="cuda", help="cuda or cpu")
    args = p.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s", datefmt="%H:%M:%S")
    os.environ.setdefault("HF_HUB_DISABLE_SYMLINKS_WARNING", "1")

    app = web.Application(client_max_size=2 * MAX_AUDIO_SECONDS * SAMPLE_RATE + 4096)
    app["token"], app["port"], app["last_language"] = load_token(), args.port, "it"
    log.info("loading Whisper %s on %s ...", args.whisper, args.device)
    app["ears"] = Ears(args.whisper, args.device)
    app["oracle"] = Oracle(args.model)
    app.cleanup_ctx.extend([lifecycle, advertise])
    app.router.add_get("/", index)
    app.router.add_get("/health", health)
    app.router.add_post("/ask", ask)
    app.router.add_post("/ask_text", ask_text)
    log.info("ball token: %s  (in %s)", app["token"], CONFIG.name)
    web.run_app(app, host=args.host, port=args.port, print=lambda *_: log.info(
        "listening on http://%s:%d  (test page: http://localhost:%d/)", lan_ip(), args.port, args.port))


if __name__ == "__main__":
    main()
