// Cricket 07 in the browser: copies the player's own game folder into OPFS (once; it never leaves the
// computer), starts the recompiled build (build/recomp.js), replays its frames with WebGPU and feeds it
// input and audio.
//
// URL options: ?mute (no audio) ?fps (frame rate in the console) ?log (host log) ?autoplay
//              ?exe=<name> (the executable inside the game folder, if not the profile's default)
//              ?pads=N (game controllers to expose, default 1, at most 4)
//              ?queue=1 (frames queued ahead of the display, default 2: 1 cuts 16 ms of input latency online
//              on a machine that simulates a frame well within 16 ms, else it hitches)
//              ?netplay=<url> (the room server for online play, see server/; default: this page's origin)
//              ?join=<room code>
//              ?4x3 (the game's own 4:3 picture; by default it fills a 16:9 stage, host/widescreen.c)
//              ?touch=1 / ?touch=0 (on-screen controls; by default on when the main pointer is a finger)
//
// Online play (netplay.js, host/netplay.c): players meet in a room on the server; when the host starts,
// every page boots the game in deterministic lockstep mode with fresh saves, and only pad inputs travel.
// Each player's keyboard and first gamepad merge into one local pad; mouse and raw keys are off.
'use strict';

const $ = (id) => document.getElementById(id);
const params = new URLSearchParams(location.search);
const TOUCH = document.documentElement.classList.contains('touch');   // on-screen controls (index.html decides)
const OPFS_DIR = 'rcgame';
const MARKER = '.installed.json';   // { version } once every file is copied
const NETPLAY_BASE = new URL(params.get('netplay') || './', location.href).href.replace(/\/?$/, '/');
const status = (t) => { $('status').textContent = t; };
// the launcher's screens (index.html): loading, home, online, lobby
const show = (view) => { for (const v of document.querySelectorAll('#panel .view')) v.classList.toggle('hidden', v.id !== 'view-' + view); };
const fail = (e) => { bootDone(); status('Error: ' + e.message); $('panel').classList.remove('hidden'); show('loading'); };
const mb = (n) => (n / 1e6).toFixed(0);

// ------------------------------------------------------------------ game files in OPFS

async function gameDir(create) {
  const root = await navigator.storage.getDirectory();
  return root.getDirectoryHandle(OPFS_DIR, { create });
}

async function readMarker() {
  try {
    const f = await (await (await gameDir(false)).getFileHandle(MARKER)).getFile();
    return JSON.parse(await f.text());
  } catch { return null; }
}

async function writeMarker(version) {
  const fh = await (await gameDir(true)).getFileHandle(MARKER, { create: true });
  const w = await fh.createWritable();
  await w.write(JSON.stringify({ version, at: new Date().toISOString() }));
  await w.close();
}

async function fileHandle(dir, relPath, create) {
  const parts = relPath.split('/');
  for (const p of parts.slice(0, -1)) dir = await dir.getDirectoryHandle(p, { create });
  return dir.getFileHandle(parts[parts.length - 1], { create });
}

async function importFromPicker(fileList) {
  // the whole folder, minus its own name: <picked>/a/b.dat -> a/b.dat
  const list = [...fileList].map((f) => ({ f, dest: f.webkitRelativePath.split('/').slice(1).join('/') }))
    .filter((e) => e.dest);
  if (!list.some((e) => e.dest.toLowerCase().endsWith('.exe'))) throw new Error('No .exe in that folder.');
  const root = await navigator.storage.getDirectory();
  try { await root.removeEntry(OPFS_DIR, { recursive: true }); } catch {}
  await navigator.storage.persist?.();
  const dir = await gameDir(true);
  const total = list.reduce((s, e) => s + e.f.size, 0);
  let done = 0;
  $('progress').classList.remove('hidden');
  for (const e of list) {
    status(`Copying ${e.dest} (${mb(done)} / ${mb(total)} MB)`);
    const w = await (await fileHandle(dir, e.dest, true)).createWritable();
    await w.write(e.f);
    await w.close();
    done += e.f.size;
    $('progress').value = done / total;
  }
  const sig = list.map((e) => `${e.dest}:${e.f.size}`).sort().join('\n');
  const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(sig));
  kickCollector('after import');
  await writeMarker([...new Uint8Array(digest)].slice(0, 6).map((b) => b.toString(16).padStart(2, '0')).join(''));
  $('progress').classList.add('hidden');
}

// ------------------------------------------------------------------ input

// KeyboardEvent.code -> USB HID usage (what host/input.c's key table uses)
const HID = {};
for (let i = 0; i < 26; i++) HID['Key' + String.fromCharCode(65 + i)] = 4 + i;
for (let i = 1; i <= 9; i++) HID['Digit' + i] = 29 + i;
HID.Digit0 = 39;
Object.assign(HID, {
  Enter: 40, Escape: 41, Backspace: 42, Tab: 43, Space: 44, Minus: 45, Equal: 46, BracketLeft: 47, BracketRight: 48,
  Backslash: 49, Semicolon: 51, Quote: 52, Backquote: 53, Comma: 54, Period: 55, Slash: 56, CapsLock: 57,
  PrintScreen: 70, ScrollLock: 71, Pause: 72, Insert: 73, Home: 74, PageUp: 75, Delete: 76, End: 77, PageDown: 78,
  ArrowRight: 79, ArrowLeft: 80, ArrowDown: 81, ArrowUp: 82, NumLock: 83, NumpadDivide: 84, NumpadMultiply: 85,
  NumpadSubtract: 86, NumpadAdd: 87, NumpadEnter: 88, Numpad0: 98, NumpadDecimal: 99,
  ControlLeft: 224, ShiftLeft: 225, AltLeft: 226, MetaLeft: 227, ControlRight: 228, ShiftRight: 229, AltRight: 230, MetaRight: 231,
});
for (let i = 1; i <= 12; i++) HID['F' + i] = 57 + i;
for (let i = 1; i <= 9; i++) HID['Numpad' + i] = 88 + i;

// Netplay: the keyboard drives this player's pad (the game itself gets no keys or mouse, which would desync).
// Bit 4 is LB, which the game sees as L1: its modifier for lofted shots (the in-game tutorial: "L1 + shot
// button" to hit a six; on the keyboard that's Shift), so Shift holds it as well as Q. The game pauses only
// on the keyboard's Esc, not the pad's Start, so Esc is bit 20: host/netplay.c applies it to the DirectInput
// keyboard on every peer, with the pad inputs (NP_KEY_ESC). Backspace is B, back in menus.
const NET_KEYS = {   // KeyboardEvent.code -> standard-gamepad bit (20: Esc key)
  Enter: 0, Space: 0, KeyS: 0, Escape: 20, Backspace: 1, KeyD: 1, KeyA: 2, KeyW: 3, KeyQ: 4, KeyE: 5,
  ShiftLeft: 4, ShiftRight: 4, AltLeft: 5, AltRight: 5,
  Digit1: 6, Digit3: 7, Tab: 8, KeyP: 9, ArrowUp: 12, ArrowDown: 13, ArrowLeft: 14, ArrowRight: 15,
};
let netKeyBits = 0;
let netKeyPressed = 0;   // keys pressed since the last poll, so a tap shorter than a frame still counts

function hookInput(M, net) {
  const canvas = $('canvas');
  if (net) {
    const press = (code, down) => {
      const bit = NET_KEYS[code];
      if (bit === undefined) return false;
      if (down) { netKeyBits |= 1 << bit; netKeyPressed |= 1 << bit; } else netKeyBits &= ~(1 << bit);
      pollLocalPad(M);   // straight to the game: waiting for the next animation frame would add up to 16 ms
      return true;
    };
    const key = (down) => (e) => { if (press(e.code, down)) e.preventDefault(); };
    window.addEventListener('keydown', key(true));
    window.addEventListener('keyup', key(false));
    window.addEventListener('blur', () => { netKeyBits = 0; pollLocalPad(M); });
    canvas.addEventListener('contextmenu', (e) => e.preventDefault());
    if (TOUCH) setupTouch(press);
    return;
  }
  const press = (code, down) => {
    const u = HID[code];
    if (u === undefined) return false;
    M._input_key_hid(u, down ? 1 : 0);
    return true;
  };
  const key = (down) => (e) => {
    if (HID[e.code] === undefined || e.repeat) return;
    if (!(e.metaKey && !down)) e.preventDefault();
    press(e.code, down);
  };
  if (TOUCH) setupTouch(press);
  window.addEventListener('keydown', key(true));
  window.addEventListener('keyup', key(false));
  // The game draws its own cursor and reads relative motion. Lock the pointer when the browser allows
  // it (then the system cursor can't leave the game); without a lock, motion over the canvas still works.
  // Touch on the game's screen: a tap clicks where the finger is and a drag drags the cursor (host/input.c:
  // input_tap, since the game's cursor only moves by relative motion). The browser's mouse events that follow
  // a touch are ignored, as their motion is in screen points rather than the game's pixels. With the on-screen
  // controls (TOUCH) taps on the game do nothing: a thumb that slips off a control would move the cursor.
  let tapId = null, tapLast = null, touchAt = -1e9;
  const gameArea = () => {   // the game's 640x480 screen inside the canvas (widescreen: full height, centred)
    const r = canvas.getBoundingClientRect();
    const h = r.width * 3 > r.height * 4 ? r.height : r.width * 3 / 4, w = h * 4 / 3;
    return { x: r.left + (r.width - w) / 2, y: r.top + (r.height - h) / 2, scale: 480 / h };
  };
  canvas.addEventListener('pointerdown', (e) => {
    if (e.pointerType === 'mouse') return;
    e.preventDefault();
    touchAt = e.timeStamp;
    if (TOUCH) return;
    tapId = e.pointerId;
    try { canvas.setPointerCapture(e.pointerId); } catch { /* the pointer is already gone */ }
    const a = gameArea();
    tapLast = { x: e.clientX, y: e.clientY, fx: 0, fy: 0 };
    M._input_tap(Math.max(0, Math.min(639, Math.round((e.clientX - a.x) * a.scale))),
      Math.max(0, Math.min(479, Math.round((e.clientY - a.y) * a.scale))));
  });
  canvas.addEventListener('pointermove', (e) => {
    if (e.pointerId !== tapId) return;
    const s = gameArea().scale;
    tapLast.fx += (e.clientX - tapLast.x) * s; tapLast.fy += (e.clientY - tapLast.y) * s;
    tapLast.x = e.clientX; tapLast.y = e.clientY;
    const dx = Math.trunc(tapLast.fx), dy = Math.trunc(tapLast.fy);
    tapLast.fx -= dx; tapLast.fy -= dy;
    if (dx || dy) M._input_tap_move(dx, dy);
  });
  for (const t of ['pointerup', 'pointercancel'])
    canvas.addEventListener(t, (e) => {
      if (e.pointerType !== 'mouse') touchAt = e.timeStamp;
      if (e.pointerId === tapId) { tapId = null; M._input_tap_release(); }
    });
  const fromTouch = (e) => e.timeStamp - touchAt < 1000 || e.sourceCapabilities?.firesTouchEvents;
  canvas.addEventListener('mousedown', (e) => {
    if (fromTouch(e)) return;
    if (document.pointerLockElement !== canvas) {
      const p = canvas.requestPointerLock?.();
      if (p && p.catch) p.catch(() => {});
    }
    M._input_mouse_button([0, 2, 1][e.button] ?? 0, 1);
  });
  window.addEventListener('mouseup', (e) => { if (!fromTouch(e)) M._input_mouse_button([0, 2, 1][e.button] ?? 0, 0); });
  canvas.addEventListener('mousemove', (e) => { if (!fromTouch(e)) M._input_mouse_move(e.movementX, e.movementY); });
  canvas.addEventListener('contextmenu', (e) => e.preventDefault());
}

// On-screen controls for phones and tablets (#touch in index.html): they press the keys the keyboard
// controls use (the side panel's list), so they work wherever the keyboard does, online too (NET_KEYS).
// Left: in play a stick (the arrow keys; between two directions it holds both, for diagonal shots), in menus a
// d-pad (one direction at a time, the arm under the thumb; the renderer says which: rc_in_menu). Right: the
// keys themselves, labelled as on the keyboard (S, D, A, W in a diamond; Shift and Alt above), plus Esc at
// the top: it pauses, and it moves on from the innings-break highlights (where S only shows the next one).
// A quick tap on Shift or Alt latches it until the next letter key is let go (a lofted shot is Shift
// held through S, which one thumb can't do); holding it works too.
function setupTouch(press) {
  const root = $('touch');
  const buzz = () => navigator.vibrate?.(8);
  const held = new Set();   // codes down, so a lost touch (tab hidden) releases them
  const set = (code, down) => {
    if (down === held.has(code)) return;
    if (down) held.add(code); else held.delete(code);
    press(code, down);
  };

  const pad = $('tpad'), knob = pad.querySelector('.knob');
  const DIRS = [['ArrowRight'], ['ArrowRight', 'ArrowDown'], ['ArrowDown'], ['ArrowDown', 'ArrowLeft'],
    ['ArrowLeft'], ['ArrowLeft', 'ArrowUp'], ['ArrowUp'], ['ArrowUp', 'ArrowRight']];
  const ARROWS = ['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight'];
  let padPointer = null;
  const steer = (e) => {
    const r = pad.getBoundingClientRect(), rad = r.width / 2;
    let dx = (e.clientX - r.left - rad) / rad, dy = (e.clientY - r.top - rad) / rad;
    const d = Math.hypot(dx, dy);
    const want = d < 0.28 ? [] : DIRS[Math.round(Math.atan2(dy, dx) / (Math.PI / 4) + 8) % 8];
    for (const a of ARROWS) if (!want.includes(a)) set(a, false);
    if (want.some((a) => !held.has(a))) buzz();
    for (const a of want) set(a, true);
    if (d > 1) { dx /= d; dy /= d; }
    knob.style.transform = `translate(${dx * rad * 0.55}px, ${dy * rad * 0.55}px)`;
  };
  const centre = () => { padPointer = null; knob.style.transform = ''; for (const a of ARROWS) set(a, false); };

  // menus: the d-pad's arm under the thumb, whichever way is furthest from the middle
  const dpad = $('tdpad');
  let dpadPointer = null;
  const arm = (e) => {
    const r = dpad.getBoundingClientRect(), rad = r.width / 2;
    const dx = (e.clientX - r.left - rad) / rad, dy = (e.clientY - r.top - rad) / rad;
    const want = Math.max(Math.abs(dx), Math.abs(dy)) < 0.2 ? '' :
      Math.abs(dx) > Math.abs(dy) ? (dx > 0 ? 'ArrowRight' : 'ArrowLeft') : (dy > 0 ? 'ArrowDown' : 'ArrowUp');
    for (const a of ARROWS) if (a !== want) set(a, false);
    if (want && !held.has(want)) { buzz(); set(want, true); }
    for (const b of dpad.querySelectorAll('[data-dir]')) b.classList.toggle('on', b.dataset.dir === want);
  };
  const dpadUp = () => {
    dpadPointer = null;
    for (const a of ARROWS) set(a, false);
    for (const b of dpad.querySelectorAll('.on')) b.classList.remove('on');
  };
  dpad.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    dpadPointer = e.pointerId;
    dpad.setPointerCapture(e.pointerId);
    arm(e);
  });
  dpad.addEventListener('pointermove', (e) => { if (e.pointerId === dpadPointer) arm(e); });
  for (const t of ['pointerup', 'pointercancel', 'lostpointercapture'])
    dpad.addEventListener(t, (e) => { if (e.pointerId === dpadPointer) dpadUp(); });
  // switching between them lets go of the arrows (the renderer's verdict holds for a few frames first)
  document.addEventListener('c07-menu', () => { centre(); dpadUp(); });
  pad.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    padPointer = e.pointerId;
    pad.setPointerCapture(e.pointerId);
    steer(e);
  });
  pad.addEventListener('pointermove', (e) => { if (e.pointerId === padPointer) steer(e); });
  for (const t of ['pointerup', 'pointercancel', 'lostpointercapture'])
    pad.addEventListener(t, (e) => { if (e.pointerId === padPointer) centre(); });

  const latched = new Set();   // shoulder codes held by a tap
  for (const b of root.querySelectorAll('[data-code]')) {
    const code = b.dataset.code, shoulder = b.classList.contains('shoulder');
    let since = 0, unlatch = false;
    const light = () => b.classList.toggle('on', held.has(code));
    b.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      b.setPointerCapture(e.pointerId);
      buzz();
      if (shoulder && latched.has(code)) { latched.delete(code); unlatch = true; set(code, false); light(); return; }
      since = e.timeStamp;
      set(code, true);
      light();
    });
    const up = (e) => {
      if (unlatch) { unlatch = false; return; }
      if (!held.has(code)) return;
      if (shoulder && e.type === 'pointerup' && e.timeStamp - since < 300) { latched.add(code); return; }
      set(code, false);
      light();
      if (!shoulder) {   // a face button let go: release the latched shoulders
        for (const s of latched) { set(s, false); root.querySelector(`[data-code="${s}"]`).classList.remove('on'); }
        latched.clear();
      }
    };
    b.addEventListener('pointerup', up);
    b.addEventListener('pointercancel', up);
    b.addEventListener('contextmenu', (e) => e.preventDefault());
  }

  const releaseAll = () => {
    centre();
    for (const c of [...held]) set(c, false);
    latched.clear();
    for (const b of root.querySelectorAll('.on')) b.classList.remove('on');
  };
  document.addEventListener('visibilitychange', () => { if (document.hidden) releaseAll(); });
  document.addEventListener('c07-release', releaseAll);
  window.addEventListener('blur', releaseAll);
}

// In menus the on-screen controls show a d-pad instead of the stick (html.menu). The renderer's verdict
// (rc_in_menu, host/render_rec.c) has to hold for 8 frames, so a frame or two of a fade doesn't flip it.
let menuMode = null, menuSeen = null, menuFrames = 0;
function touchMenuMode(M) {
  const m = !!M._rc_in_menu();
  if (m !== menuSeen) { menuSeen = m; menuFrames = 0; }
  if (m === menuMode || ++menuFrames < 8) return;
  menuMode = m;
  document.documentElement.classList.toggle('menu', m);
  document.dispatchEvent(new Event('c07-menu'));
}

// Gamepads (Gamepad API, polled every animation frame). Browsers only expose a pad after a button press
// on it. The game sees each pad as a DirectInput "Logitech Dual Action USB" (host/dinput.c); ?pads=N exposes
// N of them (default 1, at most 4). Like the native feeder (host/window.c), slots are DirectInput pad
// numbers handed out in connection order, not the browser's gamepad.index (which can start above 0, e.g.
// after a reconnect), and a pad keeps its slot until it is unplugged.
// With mapping 'standard' buttons and axes are already in the layout input_gamepad() takes. Other pads
// (older DirectInput-era ones such as the Logitech Precision) report buttons in HID order, as Windows
// DirectInput presents them and as the game's controltypes.csv expects; their axes are indexed by HID
// usage (X Y Z Rx Ry Rz ...), so the right stick is Z/Rz and Chrome puts the hat switch at axes[9].
const PAD_SLOTS = 4;
const padLast = Array.from({ length: PAD_SLOTS }, () => '');
const padSlotIndex = Array(PAD_SLOTS).fill(-1);   // slot -> gamepad.index
const padHats = new Set();   // gamepad.index of pads whose axes[9] is a hat switch
// raw button n -> standard-layout bit that host/dinput.c turns back into DirectInput button n
const STD_FOR_RAW = [2, 0, 1, 3, 4, 5, 6, 7, 9, 8, 10, 11];
// Chrome's hat axis: -1 = up, then clockwise in steps of 2/7; anything outside [-1, 1] is centred
const HAT_DIRS = [1 << 12, (1 << 12) | (1 << 15), 1 << 15, (1 << 13) | (1 << 15), 1 << 13, (1 << 13) | (1 << 14), 1 << 14, (1 << 12) | (1 << 14)];
const axis16 = (v) => Math.round(Math.max(-1, Math.min(1, v || 0)) * 32767);

function readPad(g) {
  let buttons = 0;
  if (g.mapping === 'standard') {
    g.buttons.forEach((b, i) => { if (i < 17 && (b.pressed || b.value > 0.3)) buttons |= 1 << i; });
    return { buttons, ax: [0, 1, 2, 3].map((i) => axis16(g.axes[i])) };
  }
  g.buttons.forEach((b, i) => { if (i < STD_FOR_RAW.length && (b.pressed || b.value > 0.3)) buttons |= 1 << STD_FOR_RAW[i]; });
  // axes[9] is only taken as a hat once it has shown the centred value, so a plain axis resting at 0 isn't
  // read as d-pad down
  const hat = g.axes[9];
  if (hat > 1.1) padHats.add(g.index);
  else if (padHats.has(g.index) && hat >= -1.01) buttons |= HAT_DIRS[Math.round((hat + 1) * 3.5) % 8];
  const ry = g.axes.length > 5 ? g.axes[5] : g.axes[3];
  return { buttons, ax: [axis16(g.axes[0]), axis16(g.axes[1]), axis16(g.axes[2]), axis16(ry)] };
}

// Netplay: one local pad, the first connected gamepad merged with the keyboard (host/netplay.c sends it).
// Called from the key handlers and from a 1 ms timer (not just the animation frame), so a change reaches
// the game thread within a few ms wherever it is in its frame.
let netLast = '';
function pollLocalPad(M) {
  const g = (navigator.getGamepads ? [...navigator.getGamepads()] : []).find((p) => p && p.connected);
  const { buttons, ax } = g ? readPad(g) : { buttons: 0, ax: [0, 0, 0, 0] };
  const b = buttons | netKeyBits | netKeyPressed;
  netKeyPressed = 0;
  const key = `${b}:${ax.join(',')}`;
  if (key === netLast) return;
  netLast = key;
  M._input_gamepad(0, 1, b, ax[0], ax[1], ax[2], ax[3]);
}

function pollGamepads(M) {
  const pads = navigator.getGamepads ? [...navigator.getGamepads()].filter((g) => g && g.connected) : [];
  // free the slots of unplugged pads, then give new pads the lowest free slot
  for (let s = 0; s < PAD_SLOTS; s++)
    if (padSlotIndex[s] >= 0 && !pads.some((g) => g.index === padSlotIndex[s])) { padHats.delete(padSlotIndex[s]); padSlotIndex[s] = -1; }
  for (const g of pads) {
    if (padSlotIndex.includes(g.index)) continue;
    const s = padSlotIndex.indexOf(-1);
    if (s < 0) break;
    padSlotIndex[s] = g.index;
    console.log(`gamepad ${g.index} -> pad ${s}: ${g.id} (${g.mapping || 'non-standard'} mapping, ${g.buttons.length} buttons, ${g.axes.length} axes)`);
  }
  for (let slot = 0; slot < PAD_SLOTS; slot++) {
    const g = pads.find((p) => p.index === padSlotIndex[slot]);
    const { buttons, ax } = g ? readPad(g) : { buttons: 0, ax: [0, 0, 0, 0] };
    const connected = g ? 1 : 0;
    const key = `${connected}:${buttons}:${ax.join(',')}`;
    if (key === padLast[slot]) continue;
    padLast[slot] = key;
    M._input_gamepad(slot, connected, buttons, ax[0], ax[1], ax[2], ax[3]);
  }
}

// ------------------------------------------------------------------ audio

async function startAudio(M) {
  if (params.has('mute')) return;
  const ctx = new AudioContext({ sampleRate: 44100, latencyHint: 'interactive' });
  await ctx.audioWorklet.addModule('audio-worklet.js');
  const node = new AudioWorkletNode(ctx, 'rc-ring', { outputChannelCount: [2] });
  node.port.postMessage({ memory: M.wasmMemory, ring: M._rc_audio_ring() });
  node.connect(ctx.destination);
  const resume = () => ctx.resume();
  window.addEventListener('pointerdown', resume);
  window.addEventListener('keydown', resume);
}

// ------------------------------------------------------------------ run

function sizeCanvas() {
  const canvas = $('canvas');
  const r = canvas.getBoundingClientRect(), dpr = Math.min(window.devicePixelRatio || 1, 2);
  // at least 640x480, scaled up evenly so the picture keeps the canvas's shape (a phone's stage is small)
  const w = Math.max(1, r.width * dpr), h = Math.max(1, r.height * dpr), s = Math.max(1, 640 / w, 480 / h);
  canvas.width = Math.round(w * s);
  canvas.height = Math.round(h * s);
}

// A netplay session: every peer starts from the same empty saves folder, kept apart from this browser's
// normal saves (one per player slot, so two tabs of one browser can play each other).
const netSaves = (session) => `rcnetplay-${session.you}`;
async function clearNetSaves(session) {
  const root = await navigator.storage.getDirectory();
  try { await root.removeEntry(netSaves(session), { recursive: true }); } catch { /* not there yet */ }
}

function showNetStatus(M, session) {
  const el = $('netstatus');
  el.classList.remove('hidden');
  let lastFrame = 0, lastT = performance.now();
  setInterval(() => {
    const frame = M._netplay_status(0), desync = M._netplay_status(2), stalls = M._netplay_status(3);
    const now = performance.now();
    // the game thread always idles until the next frame's inputs arrive; it's only "waiting" when that
    // holds the game below full speed
    const fps = ((frame - lastFrame) * 1000) / Math.max(1, now - lastT);
    lastFrame = frame; lastT = now;
    el.classList.toggle('bad', !!desync);
    // the round trip the inputs ride on, and how many frames the delay failed to cover (a stall is a
    // frame that waited over 10 ms for the others' inputs: the game froze that long)
    const trips = session.client.trips();
    const path = trips.direct ? `direct ${trips.direct} ms` : `relayed ${trips.room} ms`;
    el.textContent = desync ? `Out of sync since frame ${desync}: restart the session`
      : `Online · player ${session.you + 1} of ${session.players} · delay ${session.delay}` +
        ` · ${path}` + (stalls ? ` · ${stalls} stalls` : '') +
        (session.reconnecting ? ' · reconnecting…' : frame > 300 && fps < 50 ? ' · waiting for players' : '') +
        (session.left?.size ? ` · ${session.left.size} left` : '');
  }, 1000);
}

// The game's code (30 MB of WebAssembly) downloads and compiles while the launcher shows, so Play starts sooner;
// run() hands the compiled module to the build (instantiateWasm), and its threads reuse it.
let wasmModule = null;
function precompile() {
  if (wasmModule || !WebAssembly.compileStreaming) return;
  wasmModule = WebAssembly.compileStreaming(fetch('build/recomp.wasm'));
  wasmModule.catch(() => { wasmModule = null; });   // run() falls back to the build's own loading
}

// From Play until the game runs, a loading screen (#boot) instead of a black one. The first frame the game
// presents is the EA intro's (a movie: its frames are surface copies, not draws), so the screen goes at that
// first frame.
const BOOT_STEPS = { code: 'Loading the game…', gpu: 'Starting the graphics…', data: 'Loading the game data…' };
let bootTimer = 0;
function bootStep(step) {
  document.documentElement.classList.add('booting');
  $('boot').classList.remove('hidden');
  $('boot-msg').textContent = BOOT_STEPS[step];
  if (!bootTimer) bootTimer = setTimeout(() => $('boot-hint').classList.remove('hidden'), 12000);
}
function bootDone() {
  clearTimeout(bootTimer);
  document.documentElement.classList.remove('booting');
  $('boot').classList.add('hidden');
}

async function run(session) {
  bootStep('code');
  $('panel').classList.add('hidden');
  document.documentElement.classList.add('playing');   // index.html: the touch layout and controls
  updateGate();
  fullscreenTip();
  // Online play keeps 4:3 (widescreen changes the game's culling, so every peer must agree): the stage too.
  if (session) document.documentElement.classList.remove('wide');
  if (session) await clearNetSaves(session);
  sizeCanvas();
  const script = document.createElement('script');
  script.src = 'build/recomp.js';
  await new Promise((ok, fail) => { script.onload = ok; script.onerror = fail; document.head.appendChild(script); });
  precompile();
  const module = await wasmModule?.catch(() => null);
  const M = await createRecomp({
    arguments: ['/opfs/' + OPFS_DIR],
    ...(module && { instantiateWasm: (imports, receive) => {
      WebAssembly.instantiate(module, imports).then((inst) => receive(inst, module));
      return {};
    } }),
    print: (t) => console.log(t),
    printErr: (t) => console.log(t),
    // Quit in the game's main menu (or a fatal error) ends it: back to the launcher, without the options that
    // would start a game straight away (host/kernel32.c: host_exit)
    rcGameExit: () => {
      if (!params.has('join') && !params.has('autoplay')) { location.reload(); return; }
      const u = new URL(location.href);
      for (const k of ['join', 'autoplay']) u.searchParams.delete(k);
      location.replace(u);
    },
    preRun: [(m) => {
      if (params.has('mute')) m.ENV.RC_NO_AUDIO = '1';
      if (params.get('exe')) m.ENV.RC_EXE = params.get('exe');
      if (params.has('log')) m.ENV.RC_LOG = '1';
      if (params.has('pads')) m.ENV.RC_PADS = params.get('pads');
      if (params.has('queue')) m.ENV.RC_QUEUE = params.get('queue');
      if (!session && !params.has('4x3')) m.ENV.RC_WIDESCREEN = '1';
      if (session) Object.assign(m.ENV, RcNetplay.gameEnv(session), { RC_USERDATA: '/opfs/' + netSaves(session) });
      m.ENV.RC_LOG_FPS = params.has('fps') ? '1' : '';
      if (!params.has('fps')) delete m.ENV.RC_LOG_FPS;
    }],
  });
  bootStep('gpu');
  const canvas = $('canvas');
  M._rc_gpu_init(canvas.width, canvas.height);
  new ResizeObserver(() => { sizeCanvas(); M._rc_set_canvas_size(canvas.width, canvas.height); }).observe(canvas);
  hookInput(M, !!session);
  if (session) { session.client.attach(M); showNetStatus(M, session); }
  startAudio(M).catch((e) => console.warn('audio unavailable:', e));
  const poll = session ? pollLocalPad : pollGamepads;
  // Online, present at most 60 times a second: every Present is a lockstep frame, and the input delay and
  // the time a frame has to simulate are counted in those (netplay.js: autoDelay), so following a 120 Hz
  // display would halve both. Solo play follows the display. A frame is due every 1/60 s, with a little
  // tolerance so a 60 Hz display's own jitter doesn't skip one.
  let due = 0;
  const shown = { any: false, done: false };   // until the game runs (bootDone)
  const presented = () => {
    if (!shown.any) { shown.any = true; bootStep('data'); }
    shown.done = true; bootDone();
  };
  const frame = (t) => {
    poll(M);
    if (TOUCH) touchMenuMode(M);
    if (!session || t >= due - 4) {
      if (M._rc_gpu_pump(1)) { due = Math.max(due + 1000 / 60, t - 4); if (!shown.done) presented(); }
    } else M._rc_gpu_pump(0);   // mid-frame readbacks only
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);
  // readbacks mid-frame are answered without waiting for vsync; online, the pad is also sampled here
  setInterval(() => { if (session) poll(M); M._rc_gpu_pump(0); }, 1);
  // Online, a hidden tab still has to present frames (browsers pause requestAnimationFrame there), or the
  // lockstep would freeze every other player with it.
  if (session) setInterval(() => { if (document.hidden) { poll(M); M._rc_gpu_pump(1); } }, 16);
  canvas.focus();
  window.recomp = M;
  setInterval(() => kickCollector('periodic'), 90000);
}

// Every iOS browser is WebKit, so switching browsers can't help: WebGPU is on by default from iOS 26, and on
// iOS 17-18 only behind a Safari feature flag, which doesn't reach Chrome and other apps (their WKWebView). The OS
// version can't decide it: Safari 26 and Chrome freeze it in the user agent (iPhone OS 18_6).
const IOS = /iPhone|iPad|iPod/.test(navigator.userAgent) || (/Macintosh/.test(navigator.userAgent) && navigator.maxTouchPoints > 1);
function noWebGPUiOS() {
  const ua = navigator.userAgent;
  const other = /CriOS/.test(ua) ? 'Chrome' : /FxiOS/.test(ua) ? 'Firefox' : /EdgiOS/.test(ua) ? 'Edge' : '';
  const safari = !other && /Version\/[\d.]+ .*Safari\//.test(ua) && !/OPiOS|GSA\//.test(ua);
  $('status').innerHTML = 'Cricket 07 needs <b>WebGPU</b>, which iPhone and iPad only support from <b>iOS 26</b>. ' +
    'Here\'s what you can do:<ol class="options">' +
    '<li><span><b>Update to iOS 26</b> in <b>Settings → General → Software Update</b>, then open the game again.</span></li>' +
    '<li><span><b>Don\'t want to update?</b> Turn on <b>WebGPU</b> in <b>Settings → Apps → Safari → Advanced → Feature Flags</b> ' +
      '(iOS 17: <b>Settings → Safari → Advanced → Feature Flags</b>), then ' +
      (safari ? 'reload this page in Safari.' : `open the game in <b>Safari</b>, not ${other || 'this app'}: the setting only works in Safari.`) +
      ' This early WebGPU may not run the game.</span></li></ol>' +
    '<span class="small">Already on iOS 26? Lockdown Mode can switch WebGPU off.</span>';
}

// navigator.gpu with no adapter: usually hardware acceleration switched off (the browser then renders WebGL in
// software, which names itself), else a blocklisted GPU or driver, Remote Desktop or a VM. Pages can't link to
// chrome:// and friends, so the settings page is shown with a copy button.
function softwareRendering() {
  const gl = document.createElement('canvas').getContext('webgl');
  if (!gl) return true;
  const info = gl.getExtension('WEBGL_debug_renderer_info');
  return /SwiftShader|llvmpipe|softpipe|Software|Basic Render/i.test(gl.getParameter(info ? info.UNMASKED_RENDERER_WEBGL : gl.RENDERER));
}
function noAdapter() {
  const ua = navigator.userAgent;
  const [name, page, where, label] = /Firefox\//.test(ua) ? ['Firefox', 'about:preferences', 'Settings → General → Performance',
      'untick <b>Use recommended performance settings</b>, then tick <b>Use hardware acceleration when available</b>']
    : /Edg\//.test(ua) ? ['Edge', 'edge://settings/system', 'Settings → System and performance', 'turn on <b>Use graphics acceleration when available</b>']
    : /OPR\//.test(ua) ? ['Opera', 'opera://settings/system', 'Settings → Browser → System', 'turn on <b>Use hardware acceleration when available</b>']
    : navigator.brave ? ['Brave', 'brave://settings/system', 'Settings → System', 'turn on <b>Use graphics acceleration when available</b>']
    : /Chrome\//.test(ua) ? ['Chrome', 'chrome://settings/system', 'Settings → System', 'turn on <b>Use graphics acceleration when available</b>']
    : [];
  const off = softwareRendering();
  const lead = off ? `Cricket 07 needs your graphics card, but <b>hardware acceleration is off</b> in ${name || 'this browser'}.`
    : 'Cricket 07 needs WebGPU, and this browser found no graphics card it can use. Hardware acceleration may be off.';
  const steps = name ? '<ol>' +
      `<li><span>Open <b>${where}</b>, or paste this into the address bar: <code>${page}</code> ` +
        '<button class="btn ghost copy" type="button">Copy</button></span></li>' +
      `<li><span>${label[0].toUpperCase() + label.slice(1)}.</span></li>` +
      `<li><span>${name === 'Firefox' ? '<b>Restart Firefox</b>' : `Click <b>Relaunch</b> (or restart ${name})`}, then open the game again.</span></li></ol>`
    : ' Turn on hardware (graphics) acceleration in the browser settings and restart the browser.';
  $('status').innerHTML = lead + steps + '<span class="small">' +
    (name === 'Firefox' ? 'Firefox only enables WebGPU for some graphics cards and drivers; Chrome or Edge usually work. ' : '') +
    (off ? '' : 'If it\'s already on, update your graphics driver. ') +
    'Remote Desktop and virtual machines often have no graphics card for the browser.</span>';
  const copy = $('status').querySelector('.copy');
  if (copy) copy.onclick = async () => {
    try { await navigator.clipboard.writeText(page); copy.textContent = 'Copied'; } catch { copy.textContent = 'Select it'; }
  };
}

async function boot() {
  if (!crossOriginIsolated) { status('This page needs cross-origin isolation (COOP/COEP headers; see web/serve.py).'); return; }
  if (!navigator.gpu) {
    if (IOS) noWebGPUiOS();
    else status('This browser has no WebGPU. Use a current Chrome, Edge or Safari.');
    return;
  }
  // navigator.gpu can exist with no usable adapter (GPU or driver blocklisted, hardware acceleration off,
  // Remote Desktop / VM): say so instead of starting a game that can only show a black screen
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' }).catch(() => null) ||
    await navigator.gpu.requestAdapter().catch(() => null) ||
    await navigator.gpu.requestAdapter({ forceFallbackAdapter: true }).catch(() => null);
  if (!adapter) { noAdapter(); return; }
  if (adapter.isFallbackAdapter || adapter.info?.isFallbackAdapter)
    console.warn('WebGPU: only a software (fallback) adapter is available; the game will be slow');
  if (!navigator.storage?.getDirectory) { status('This browser has no Origin Private File System.'); return; }
  let onlineReady = false;
  const goOnline = () => {
    if (!onlineReady) { setupOnline(); onlineReady = true; }
    show('online');
    if (!TOUCH) ($('np-code').value ? $('np-join') : $('np-name')).focus();   // a phone would raise its keyboard
  };
  const ready = () => {
    show('home');
    precompile();
    if (params.get('join')) goOnline();   // an invite link goes straight to joining
    if (params.has('autoplay')) $('play').click();
  };
  $('play').onclick = () => run().catch(fail);
  $('go-online').onclick = goOnline;
  for (const b of document.querySelectorAll('[data-go]')) b.onclick = () => show(b.dataset.go);
  $('folder').onchange = async (e) => {
    $('pick').classList.add('hidden');
    try { await importFromPicker(e.target.files); ready(); }
    catch (err) { status(err.message); $('pick').classList.remove('hidden'); }
  };
  const showPicker = (msg) => {
    status(msg);
    $('pick').classList.remove('hidden');
    $('help').classList.remove('hidden');
  };

  $('reset').onclick = (e) => { e.preventDefault(); show('loading'); showPicker('Choose your Cricket 07 folder.'); };
  if (!(await readMarker())) {
    showPicker('Choose your Cricket 07 folder to start.');
    return;
  }
  ready();
}

// Safari's page-thread garbage collector can stop firing after this process has written files into OPFS
// (README.md, "Memory in WebKit"): the next collection then waits for gigabytes of new allocation, and
// the game's per-frame garbage grows until iOS kills the page at 2 GB. Nothing a page can call collects
// directly, but allocation counts: ArrayBuffers detached the moment they are made push the allocation total
// past the threshold while nothing stays resident, and a WeakRef shows when the collection has happened.
// A healthy heap collects after the first buffer or two. WebKit only (every iOS browser): elsewhere the
// collectors keep up by themselves. Resolves true when a collection was seen.
const WEBKIT = navigator.vendor?.startsWith('Apple');
let kicking = null;
function kickCollector(label) {
  if (!WEBKIT || !ArrayBuffer.prototype.transfer || typeof WeakRef === 'undefined') return Promise.resolve(false);
  if (kicking) return kicking;
  kicking = (async () => {
    let probe = { kick: 1 }; const ref = new WeakRef(probe); probe = null;
    const t0 = performance.now();
    let n = 0;
    for (; n < 32; n++) {   // rounds of 512 MB (64 MB at a time), up to 16 GB of churn
      try { for (let i = 0; i < 8; i++) new ArrayBuffer(64 << 20).transfer(0); } catch { break; }
      await new Promise((r) => setTimeout(r, 60));   // the collector runs concurrently: give it a moment
      if (!ref.deref()) break;
    }
    await new Promise((r) => setTimeout(r, 50));
    const ok = !ref.deref();
    if (n > 0 || !ok) console.log(`[gc] ${label}: ${ok ? 'collected' : 'no collection'} after ${(n + 1) * 512} MB of churn, ${(performance.now() - t0).toFixed(0)} ms`);
    return ok;
  })().finally(() => { kicking = null; });
  return kicking;
}

// ------------------------------------------------------------------ fullscreen

// The stage (game + overlays) goes fullscreen; the canvas grows with it and the game fills it (widescreen) or
// letterboxes its 4:3 picture inside. Alt+Enter (Option+Enter on a Mac) toggles it, as in PC games of the time; it is caught
// before the game's own key handling so Enter doesn't also reach a menu.
const CAN_FULLSCREEN = !!($('stage').requestFullscreen || $('stage').webkitRequestFullscreen);
const STANDALONE = !!navigator.standalone || matchMedia('(display-mode: fullscreen), (display-mode: standalone)').matches;
const fullscreenActive = () => !!(document.fullscreenElement || document.webkitFullscreenElement);
function enterFullscreen() {
  const stage = $('stage');
  const p = Promise.resolve((stage.requestFullscreen || stage.webkitRequestFullscreen).call(stage));
  // a phone turns to landscape and stays there while fullscreen (Android; iOS has no lock)
  if (TOUCH) p.then(() => screen.orientation?.lock?.('landscape')).catch(() => {});
  return p;
}

function setupFullscreen() {
  const stage = $('stage');
  const active = fullscreenActive;
  const toggle = () => {
    if (active()) (document.exitFullscreen || document.webkitExitFullscreen).call(document);
    else enterFullscreen().catch(() => {});
    $('canvas').focus();
  };
  if (!CAN_FULLSCREEN) { $('fs').classList.add('hidden'); return; }
  $('fs').onclick = toggle;
  const swallow = (e) => {
    if (e.code !== 'Enter' && e.code !== 'NumpadEnter') return;
    if (!e.altKey) return;
    e.preventDefault();
    e.stopImmediatePropagation();
    if (e.type === 'keydown' && !e.repeat) toggle();
  };
  window.addEventListener('keydown', swallow, true);
  window.addEventListener('keyup', swallow, true);
  // Esc pauses the game, but in fullscreen the browser takes it to exit fullscreen first. Where the Keyboard
  // Lock API exists (Chromium), locking Esc while fullscreen hands a tap of it to the game and the browser
  // asks the player to hold Esc to exit instead. Elsewhere (and in a frame, where the lock is refused) Esc
  // still leaves fullscreen as before.
  const kbd = navigator.keyboard;
  let locked = false;
  const title = () => {
    $('fs').title = active() ? `Exit fullscreen (Alt+Enter or ${locked ? 'hold ' : ''}Esc)` : 'Fullscreen (Alt+Enter)';
  };
  const sync = () => {
    if (active() && !locked && kbd?.lock) {
      kbd.lock(['Escape']).then(() => {
        if (active()) { locked = true; title(); } else kbd.unlock();
      }, () => {});
    } else if (!active() && locked) { kbd.unlock(); locked = false; }
    $('fs-icon').setAttribute('d', active() ? 'M9 4v5H4M15 4v5h5M9 20v-5H4M15 20v-5h5' : 'M4 9V4h5M20 9V4h-5M4 15v5h5M20 15v5h-5');
    title();
  };
  document.addEventListener('fullscreenchange', sync);
  document.addEventListener('webkitfullscreenchange', sync);
  // the button shows while the mouse moves over the stage and fades after a few still seconds
  let idle;
  const wake = () => { stage.classList.add('awake'); clearTimeout(idle); idle = setTimeout(() => stage.classList.remove('awake'), 2500); };
  stage.addEventListener('mousemove', wake);
  stage.addEventListener('mouseleave', () => stage.classList.remove('awake'));
  wake();
}

// once the game starts: a short hint, so players learn the shortcut
function fullscreenTip() {
  const tip = $('fstip');
  if (TOUCH || document.fullscreenElement || document.webkitFullscreenElement || $('fs').classList.contains('hidden')) return;
  tip.classList.remove('hidden');
  tip.style.opacity = '1';
  setTimeout(() => { tip.style.opacity = '0'; }, 5000);
  setTimeout(() => tip.classList.add('hidden'), 5500);
}

// ------------------------------------------------------------------ online lobby

async function buildVersion() {
  // peers must run the same build and game files, or their simulations diverge
  const marker = await readMarker();
  const wasm = await fetch('build/recomp.wasm', { method: 'HEAD' }).catch(() => null);
  // the ETag comes back weak (W/"...") when the response is compressed: compare only its value
  const etag = (wasm?.headers.get('ETag') || '').replace(/^W\//, '').replace(/"/g, '');
  return `${marker?.version || 'local'}:${etag || wasm?.headers.get('Content-Length') || '?'}`;
}

function setupOnline() {
  const msg = (t) => { $('np-msg').textContent = t; $('np-lobbymsg').textContent = t; };
  try {
    $('np-name').value = localStorage.getItem('rc-name') || '';
    const region = localStorage.getItem('rc-region');
    if ([...$('np-region').options].some((o) => o.value === region)) $('np-region').value = region;
  } catch { /* storage blocked */ }
  if (params.get('join')) $('np-code').value = params.get('join').toUpperCase();
  let client = null;
  let lastPlayers = [];

  // create: the host's first connection, which places the room in `region` (the Worker's location hint)
  async function enter(code, create) {
    if (client) return;
    const name = $('np-name').value.trim() || 'Player';
    try { localStorage.setItem('rc-name', name); localStorage.setItem('rc-region', $('np-region').value); } catch { /* storage blocked */ }
    msg('Connecting…');
    let iceServers;   // from the first ticket (TURN credentials last a day)
    const roomUrl = async () => {   // a fresh ticket each time (they expire after an hour)
      const res = await fetch(NETPLAY_BASE + '_netplay_ticket', { cache: 'no-store' });
      if (!res.ok) throw new Error('Online play is not available on this server.');
      const { ticket, iceServers: ice } = await res.json();
      iceServers ??= ice;
      const url = new URL(`netplay/${code}?ticket=${encodeURIComponent(ticket)}`, NETPLAY_BASE);
      if (create) { url.searchParams.set('create', '1'); url.searchParams.set('region', $('np-region').value); create = false; }
      url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
      return url.href;
    };
    let url;
    try { url = await roomUrl(); } catch (e) { msg(e.message); return; }
    const invite = new URL(location.href);
    invite.search = `?join=${code}` + (params.get('netplay') ? `&netplay=${encodeURIComponent(params.get('netplay'))}` : '');
    $('np-room').textContent = code;
    $('invite').textContent = invite.href;
    const version = await buildVersion();
    let session = null;
    const left = new Set();
    const me = client = RcNetplay.connect({
      url, name, version, reconnectUrl: roomUrl, iceServers, relayOnly: params.get('turn') === 'only',
      onReconnect(state) { if (session) session.reconnecting = state === 'connecting'; },
      onLobby(l) {
        if (client !== me) return;   // a room this page already left
        if (l.started) { msg('That game has already started.'); leave(); return; }
        if ($('view-lobby').classList.contains('hidden')) { msg(''); show('lobby'); }
        $('np-colo').textContent = l.colo ? `Server in ${l.colo}` : '';
        $('np-players').innerHTML = '';
        const mine = l.players.find((p) => p.slot === l.you);
        const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };
        for (const p of l.players) {
          const li = el('li');
          const direct = p.slot === l.you ? 0 : Math.max(mine?.p2p?.[p.slot] || 0, p.p2p?.[l.you] || 0);
          const link = p.slot === l.you ? (p.rtt ? `${p.rtt} ms to the room` : 'Connected')
            : direct ? `Direct connection · ${direct} ms` : 'Connected through the room';
          const who = el('div', 'who');
          who.append(el('b', null, p.name), el('small', null, link));
          li.append(el('div', 'avatar', (p.name || '?').trim().charAt(0).toUpperCase() || '?'), who);
          if (p.slot === 0) li.append(el('span', 'tag', 'Host'));
          if (p.slot === l.you) li.append(el('span', 'tag', 'You'));
          $('np-players').appendChild(li);
        }
        if (l.players.length < 2) {
          const li = el('li', 'empty');
          li.append(el('div', 'avatar', '?'), el('div', 'who', 'Waiting for your friend to join…'));
          $('np-players').appendChild(li);
        }
        $('np-hostctl').classList.toggle('hidden', !l.host);
        $('np-start').disabled = l.players.length !== 2;
        if (l.players.length > 2) msg('This is a 1v1 game: only two players can be in the room.');
        lastPlayers = l.players;
        const auto = RcNetplay.autoDelay(l.players);
        $('np-delay').options[0].textContent = `Auto (${auto} frames, ${Math.round(auto * 1000 / 60)} ms)`;
        $('np-wait').classList.toggle('hidden', l.host);
      },
      onStart(s) {
        session = { ...s, client, left };
        run(session).catch(fail);
      },
      onLeft(slot) { left.add(slot); },
      onError(e) { msg(e); setTimeout(() => { if ($('np-msg').textContent === e) msg(''); }, 8000); },
      onClose() { if (!session && client === me) { client = null; show('online'); msg('Disconnected from the room.'); } },
    });
  }
  function leave() {
    const c = client;
    client = null;
    c?.close();
    show('online');
  }
  $('np-leave').onclick = () => { leave(); msg(''); };
  $('np-copy').onclick = async () => {
    const b = $('np-copy');
    try { await navigator.clipboard.writeText($('invite').textContent); b.textContent = 'Link copied'; }
    catch { b.textContent = 'Copy failed: share the code instead'; }
    setTimeout(() => { b.textContent = 'Copy invite link'; }, 2000);
  };

  const code5 = () => Array.from(crypto.getRandomValues(new Uint8Array(5)), (b) => 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'[b % 32]).join('');
  $('np-host').onclick = () => enter(code5(), true).catch((e) => msg(e.message));
  $('np-join').onclick = () => {
    const code = $('np-code').value.trim().toUpperCase();
    if (!/^[A-Z0-9]{4,12}$/.test(code)) { msg('Enter the room code from the host.'); return; }
    enter(code).catch((e) => msg(e.message));
  };
  $('np-start').onclick = () => {
    const v = $('np-delay').value;
    client?.start(v === 'auto' ? RcNetplay.autoDelay(lastPlayers) : Number(v));
  };
}

// Keyboard cheat sheet beside the stage: in-game prompts use controller glyphs (triangle/circle/×/square,
// L1, R1), so keyboard players need a reference. The CSS (index.html) reserves the panel's space on
// viewports wide enough and shrinks the stage to match; the body's .kbd-off class (set when the player
// dismisses the panel, remembered in localStorage) hides the panel and lets the stage reclaim the space.
function setupKbdHelp() {
  const off = () => { try { return localStorage.getItem('rc-kbd') === 'hide'; } catch { return false; } };
  const save = (v) => { try { localStorage.setItem('rc-kbd', v); } catch { /* storage blocked */ } };
  const apply = () => document.body.classList.toggle('kbd-off', off());
  $('kbd-close').onclick = () => { save('hide'); apply(); };
  $('kbd-show').onclick = () => { save('show'); apply(); };
  apply();
}

// Phones play fullscreen and sideways (touch devices: html.touch). The gate (#gate in index.html) covers the page
// until they do:
//   - home: no fullscreen for pages (iPhone Safari): only Add to Home Screen gives one, and the launcher stays
//     covered, since the home screen app keeps its own storage and would download the game again;
//   - fullscreen: the game runs in a browser tab that is not fullscreen (Play Solo / Multiplayer go fullscreen
//     with their tap, so this is after the player left it or the browser refused): a button goes back;
//   - rotate: the game runs and the screen is upright (iOS can't lock it; Android locks landscape).
// Embedded pages (?embed) are left alone: the framing site decides.
function gateState() {
  if (!TOUCH) return '';
  if (!STANDALONE && !CAN_FULLSCREEN) return 'home';
  if (!document.documentElement.classList.contains('playing')) return '';
  if (!STANDALONE && !fullscreenActive()) return 'fullscreen';
  if (matchMedia('(orientation: portrait)').matches) return 'rotate';
  return '';
}

// Safari hides Share in a menu in its compact layout (iOS 26: the address bar's menu button); older layouts show
// it in the toolbar. The steps name both.
const ICON = (d) => `<svg class="inline" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" ` +
  `stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${d}</svg>`;
const SHARE_ICON = ICON('<path d="M12 3v12M8 7l4-4 4 4"/><path d="M5 12v7a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2v-7"/>');
const MENU_ICON = ICON('<path d="M4 7h16M4 12h16M4 17h10"/>');
const MORE_ICON = ICON('<circle cx="5" cy="12" r="1.3" fill="currentColor"/><circle cx="12" cy="12" r="1.3" fill="currentColor"/>' +
  '<circle cx="19" cy="12" r="1.3" fill="currentColor"/>');
const GATES = {
  home: { title: 'Add Cricket 07 to your Home Screen',
    text: 'On iPhone the game plays fullscreen only from the Home Screen.<ol>' +
      `<li><span>Tap ${MENU_ICON} or ${MORE_ICON} next to the address, then <b>Share</b> (or tap ${SHARE_ICON} if Safari shows it).</span></li>` +
      '<li><span>Tap <b>Add to Home Screen</b> (under <b>More</b> if it isn\'t listed).</span></li>' +
      '<li><span>Open <b>Cricket 07</b> from your Home Screen.</span></li></ol>' },
  fullscreen: { title: 'Cricket 07 plays fullscreen', text: 'Tap below to play.', button: 'Go fullscreen' },
  rotate: { title: 'Turn your phone sideways', text: 'Cricket 07 plays in landscape.' },
};

let gateShown = '';
function updateGate() {
  const state = gateState();
  if (state === gateShown) return;
  gateShown = state;
  const gate = $('gate');
  gate.classList.toggle('hidden', !state);
  gate.dataset.state = state;
  if (!state) { $('canvas').focus(); return; }
  const g = GATES[state];
  $('gate-title').textContent = g.title;
  $('gate-text').innerHTML = g.text;
  $('gate-btn').textContent = g.button || '';
  $('gate-btn').classList.toggle('hidden', !g.button);
  document.dispatchEvent(new Event('c07-release'));   // let go of on-screen buttons the gate now covers
}

// Touch: no zooming. The viewport tag stops pinch zoom on Android; iOS ignores it, so its gesture events are
// refused, and html.touch's touch-action stops double-tap zoom.
// An iPhone home screen app (navigator.standalone) can report a viewport shorter than the screen once turned to
// landscape, leaving a band at the bottom: there the page takes its height from the screen (index.html: --app-h).
// iOS gives screen.width/height upright whatever the orientation.
function setupScreen() {
  if (!TOUCH) return;
  for (const t of ['gesturestart', 'gesturechange', 'gestureend']) document.addEventListener(t, (e) => e.preventDefault());
  if (!navigator.standalone) return;
  const fit = () => {
    const land = matchMedia('(orientation: landscape)').matches;
    const h = land ? Math.min(screen.width, screen.height) : Math.max(screen.width, screen.height);
    document.documentElement.style.setProperty('--app-h', `${Math.max(h, window.innerHeight)}px`);
  };
  for (const t of ['resize', 'orientationchange']) window.addEventListener(t, fit);
  matchMedia('(orientation: landscape)').addEventListener?.('change', fit);
  fit();
}

function setupGate() {
  if (!TOUCH) return;
  // Play Solo and Play Multiplayer go fullscreen first: it needs their tap (capture phase: before their handlers)
  $('panel').addEventListener('click', (e) => {
    if (!e.target.closest('#play, #go-online') || STANDALONE || !CAN_FULLSCREEN || fullscreenActive()) return;
    enterFullscreen().catch(() => {});
  }, true);
  $('gate-btn').onclick = () => enterFullscreen().catch(() => {});
  for (const t of ['resize', 'orientationchange']) window.addEventListener(t, updateGate);
  for (const t of ['fullscreenchange', 'webkitfullscreenchange']) document.addEventListener(t, updateGate);
  matchMedia('(orientation: portrait)').addEventListener?.('change', updateGate);
  updateGate();
}

// Android, in a browser tab: where the browser can install the page (a host that adds a web app manifest), an
// Install button for a home screen icon that opens fullscreen and in landscape.
function setupInstall() {
  if (!TOUCH || STANDALONE || !CAN_FULLSCREEN) return;
  const box = $('install');
  window.addEventListener('beforeinstallprompt', (e) => {
    e.preventDefault();
    box.innerHTML = '<b>Install Cricket 07</b> for a home screen icon that opens it fullscreen.<br>' +
      '<button class="btn" type="button">Install</button>';
    box.querySelector('button').onclick = () => { e.prompt(); e.userChoice.finally(() => box.classList.add('hidden')); };
    box.classList.remove('hidden');
  });
  window.addEventListener('appinstalled', () => box.classList.add('hidden'));
}

setupFullscreen();
setupScreen();
setupGate();
setupInstall();
setupKbdHelp();
boot();
