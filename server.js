import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath } from 'node:url';

const ROOT = path.dirname(fileURLToPath(import.meta.url));
const PORT = Number(process.env.PORT) || 3000;

// How long the end buzzer sounds when a talk runs out, in ms.
const ALARM_MS = 3000;

// A running session is abandoned if the display stays gone this long. The
// grace period is what keeps a wifi blip or a browser reconnect from killing
// a talk mid-flight — SSE reconnects within ~1s.
const DISPLAY_GRACE_MS = 8000;

/* ------------------------------------------------------------------ *
 * State
 *
 * The server owns the schedule. Every phase has an absolute epoch
 * start/end, so clients just render a countdown and never drift apart.
 * ------------------------------------------------------------------ */

const DEFAULT_CONFIG = {
  talkCount: 3,
  talkMs: 10 * 60 * 1000,
  warnMs: 60 * 1000,
  breakMs: 60 * 1000,
};

let state = idleState(DEFAULT_CONFIG);

function idleState(config) {
  return {
    status: 'idle',        // idle | running
    phase: null,           // talk | alarm | break
    talkIndex: 0,          // 0-based index of the current talk
    phaseStart: null,
    phaseEnd: null,
    pausedAt: null,        // epoch ms the countdown was frozen at
    seq: 0,                // bumped per session, so clients can key sounds
    muted: false,
    ...config,
    alarmMs: ALARM_MS,
  };
}

function config() {
  return {
    talkCount: state.talkCount,
    talkMs: state.talkMs,
    warnMs: state.warnMs,
    breakMs: state.breakMs,
  };
}

function enterPhase(phase, talkIndex, at) {
  const durations = { talk: state.talkMs, alarm: ALARM_MS, break: state.breakMs };
  state.status = 'running';
  state.phase = phase;
  state.talkIndex = talkIndex;
  state.phaseStart = at;
  state.phaseEnd = at + durations[phase];
}

// A finished session just goes back to waiting for the control — there is
// no separate "done" screen to clear.
function finish() {
  const { muted, seq } = state;
  state = idleState(config());
  state.muted = muted;
  state.seq = seq;
}

// Move out of the current phase. `skipped` means the MC pressed skip, which
// jumps past the end buzzer instead of playing it.
function advance(at, skipped = false) {
  const last = state.talkIndex >= state.talkCount - 1;

  if (state.phase === 'talk') {
    if (skipped) return last ? finish() : nextAfterTalk(at);
    return enterPhase('alarm', state.talkIndex, at);
  }

  if (state.phase === 'alarm') {
    return last ? finish() : nextAfterTalk(at);
  }

  if (state.phase === 'break') {
    return enterPhase('talk', state.talkIndex + 1, at);
  }
}

function nextAfterTalk(at) {
  if (state.breakMs > 0) return enterPhase('break', state.talkIndex, at);
  return enterPhase('talk', state.talkIndex + 1, at);
}

// Catch up on any phases whose end time has passed (also covers the case
// where several short phases elapse between ticks).
function tick() {
  if (state.status !== 'running') return false;

  // No display means nobody can see or hear the timer — drop back to setup.
  if (displayGoneAt && Date.now() - displayGoneAt > DISPLAY_GRACE_MS) {
    const seq = state.seq;
    state = idleState(config());
    state.seq = seq;
    return true;
  }

  if (state.pausedAt) return false;

  let changed = false;
  let guard = 0;
  while (state.phaseEnd && Date.now() >= state.phaseEnd && guard++ < 100) {
    advance(state.phaseEnd);
    changed = true;
  }
  return changed;
}

/* ------------------------------------------------------------------ *
 * Clients (SSE)
 * ------------------------------------------------------------------ */

let nextClientId = 1;
const clients = new Map(); // id -> { res, role }
let displayGoneAt = Date.now(); // null while at least one display is connected
let displayAudioReady = false;  // display reported the browser will let it play

function displayCount() {
  let n = 0;
  for (const c of clients.values()) if (c.role === 'display') n++;
  return n;
}

function snapshot() {
  return {
    ...state,
    now: Date.now(),
    displays: displayCount(),
    controls: clients.size - displayCount(),
    displayAudioReady,
  };
}

function send(res, event, data) {
  res.write(`event: ${event}\ndata: ${JSON.stringify(data)}\n\n`);
}

function broadcast(event = 'state', data = snapshot()) {
  for (const { res } of clients.values()) {
    try { send(res, event, data); } catch { /* client is gone; cleanup on close */ }
  }
}

function handleEvents(req, res, url) {
  const role = url.searchParams.get('role') === 'display' ? 'display' : 'control';
  res.writeHead(200, {
    'Content-Type': 'text/event-stream',
    'Cache-Control': 'no-cache, no-transform',
    Connection: 'keep-alive',
    'X-Accel-Buffering': 'no',
  });
  req.socket.setNoDelay(true);
  res.write('retry: 1000\n\n');

  const id = nextClientId++;
  clients.set(id, { res, role });
  if (role === 'display') displayGoneAt = null;
  send(res, 'state', snapshot());
  broadcast(); // let everyone else see the new connection count

  req.on('close', () => {
    clients.delete(id);
    if (displayCount() === 0) {
      displayGoneAt = Date.now();
      displayAudioReady = false;
    }
    broadcast();
  });
}

/* ------------------------------------------------------------------ *
 * Actions
 * ------------------------------------------------------------------ */

function applyAction(body) {
  const now = Date.now();

  switch (body.action) {
    case 'start': {
      if (displayCount() === 0) throw new Error('no display connected');
      const cfg = {
        talkCount: clampInt(body.talkCount, 1, 20, state.talkCount),
        talkMs: clampInt(body.talkMs, 1000, 6 * 60 * 60 * 1000, state.talkMs),
        warnMs: clampInt(body.warnMs, 0, 6 * 60 * 60 * 1000, state.warnMs),
        breakMs: clampInt(body.breakMs, 0, 6 * 60 * 60 * 1000, state.breakMs),
      };
      if (cfg.warnMs >= cfg.talkMs) cfg.warnMs = 0;
      const { muted, seq } = state;
      state = idleState(cfg);
      state.muted = muted;
      state.seq = seq + 1;
      enterPhase('talk', 0, now);
      return;
    }

    case 'skip':
      if (state.status !== 'running') return;
      state.pausedAt = null; // skipping implies carrying on
      advance(now, true);
      return;

    case 'pause': {
      if (state.status !== 'running') return;
      const wantPaused = typeof body.paused === 'boolean' ? body.paused : !state.pausedAt;
      if (wantPaused && !state.pausedAt) {
        state.pausedAt = now;
      } else if (!wantPaused && state.pausedAt) {
        // Slide the whole phase forward by however long we sat paused.
        const held = now - state.pausedAt;
        state.phaseStart += held;
        state.phaseEnd += held;
        state.pausedAt = null;
      }
      return;
    }

    case 'reset': {
      const { muted, seq } = state;
      state = idleState(config());
      state.muted = muted;
      state.seq = seq;
      // Stopping is deliberate, so cut any sound still ringing out. A session
      // that simply runs to its end is left alone to finish playing.
      broadcast('silence', {});
      return;
    }

    case 'mute':
      state.muted = typeof body.muted === 'boolean' ? body.muted : !state.muted;
      return;

    case 'displayAudio':
      displayAudioReady = body.ready !== false;
      return;

    case 'demo': {
      // One-shot event; not part of the persistent state. The display plays
      // the clip through in full.
      broadcast('demo', { sound: body.sound === 'chime' ? 'chime' : 'alert' });
      return;
    }

    default:
      throw new Error(`unknown action: ${body.action}`);
  }
}

function clampInt(value, min, max, fallback) {
  const n = Math.round(Number(value));
  if (!Number.isFinite(n)) return fallback;
  return Math.min(max, Math.max(min, n));
}

/* ------------------------------------------------------------------ *
 * HTTP
 * ------------------------------------------------------------------ */

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.mp3': 'audio/mpeg',
  '.png': 'image/png',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon',
  '.json': 'application/json; charset=utf-8',
};

const server = http.createServer((req, res) => {
  const url = new URL(req.url, `http://${req.headers.host || 'localhost'}`);

  if (url.pathname === '/api/events') return handleEvents(req, res, url);

  if (url.pathname === '/api/state') {
    return json(res, 200, snapshot());
  }

  if (url.pathname === '/api/action' && req.method === 'POST') {
    return readBody(req)
      .then((body) => {
        applyAction(body);
        tick();
        broadcast();
        json(res, 200, snapshot());
      })
      .catch((err) => json(res, 400, { error: err.message }));
  }

  // Static files
  const routes = { '/': '/index.html', '/control': '/control.html', '/display': '/index.html' };
  const rel = routes[url.pathname] || url.pathname;
  const file = path.join(ROOT, path.normalize(rel).replace(/^(\.\.[/\\])+/, ''));

  if (!file.startsWith(ROOT)) return json(res, 403, { error: 'forbidden' });

  fs.readFile(file, (err, data) => {
    if (err) return json(res, 404, { error: 'not found' });
    // Nothing here is big enough to be worth caching, and a stale sound file
    // or page is a much worse problem than re-fetching 40KB.
    res.writeHead(200, {
      'Content-Type': MIME[path.extname(file)] || 'application/octet-stream',
      'Cache-Control': 'no-store',
    });
    res.end(data);
  });
});

function json(res, code, data) {
  res.writeHead(code, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store' });
  res.end(JSON.stringify(data));
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    let raw = '';
    req.on('data', (c) => {
      raw += c;
      if (raw.length > 1e5) reject(new Error('body too large'));
    });
    req.on('end', () => {
      try { resolve(raw ? JSON.parse(raw) : {}); } catch { reject(new Error('invalid JSON')); }
    });
    req.on('error', reject);
  });
}

// Drive phase transitions and keep clients' clock offset fresh.
setInterval(() => { if (tick()) broadcast(); }, 200);
setInterval(() => broadcast('ping', { now: Date.now() }), 2000);

server.listen(PORT, '0.0.0.0', () => {
  const ips = Object.values(os.networkInterfaces())
    .flat()
    .filter((i) => i && i.family === 'IPv4' && !i.internal)
    .map((i) => i.address);

  console.log(`\n  Presentation timer running on port ${PORT}\n`);
  console.log(`  Display (stage laptop):  http://localhost:${PORT}/`);
  for (const ip of ips) {
    console.log(`  Control (MC phone):      http://${ip}:${PORT}/control`);
  }
  console.log('');
});
