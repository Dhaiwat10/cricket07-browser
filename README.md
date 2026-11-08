# Cricket 07 in the browser

EA Sports Cricket 07 (2006, Windows) running in a browser tab, with game controllers and online 1v1
multiplayer, on desktops and phones. This repo has no game files and no game code, so you need your own
copy of the game. Everything below is enough to build it and run it yourself: on your own computer, or
hosted for friends who own the game too.

## How it was made

The game's executable was translated from x86 machine code to C. Ghidra finds the functions, and
`translator/x86toc.py` turns each one into a C function: 31,548 functions, about 1.3 million
instructions. That C is compiled to WebAssembly with Emscripten, or to a native Mac app with clang.

The translated game still expects Windows, so `host/` provides the parts it uses. Direct3D 8 is
rebuilt on WebGPU, DirectSound plays through Web Audio, DirectInput reads browser keyboard and mouse
events and gamepads, and file, thread and timer calls map onto their browser equivalents. In the
browser, the game files live in the browser's private storage, copied in once from a folder you pick.

Online play works without any changes to the game: both players run the same simulation, and only
their controller inputs travel between them. Th## Self-hosting

### What you need

- **Your own copy of Cricket 07 (PC)**, installed, with an executable that has no disc copy protection. The
  retail `Cricket07.exe` is wrapped by SafeDisc, whose encrypted code can't be translated. The build checks
  the executable against `profiles/cricket07.patches.json` and stops if it doesn't match.
- **To build:** macOS on Apple Silicon, Python 3 with `capstone` and `pefile`, [Ghidra](https://ghidra-sre.org) 12,
  SDL2 (`brew install sdl2`), [wgpu-native](https://github.com/gfx-rs/wgpu-native) in `third_party/wgpu`, and
  [Emscripten](https://emscripten.org). (Only `build.sh`'s native app needs SDL2 and wgpu-native.)
- **To play:** a browser with WebGPU: current Chrome or Edge, Safari on macOS 26 or iOS 26. Phones get
  on-screen controls.

### Build

```bash
# 1. Export the function list from your executable (once, a few minutes)
GHIDRA=~/ghidra_12.1.4_PUBLIC ghidra/export.sh /path/to/Cricket07.exe work/functions.json

# 2. Translate it and build the native Mac app (PROFILE also applies profiles/cricket07.patches.json)
PROFILE=profiles/cricket07.h ./build.sh /path/to/Cricket07.exe work/functions.json
build/recomp /path/to/game-folder       # optional: the native app; RC_EXE=name.exe if yours is named differently

# 3. Build the browser version
PROFILE=profiles/cricket07.h ./build_web.sh
```

`profiles/cricket07.h` holds what the game expects to find (its install folder, registry values) and turns on
its own patches: widescreen, remembered display options, taps steering the menu cursor, and the archives
loading lazily (which keeps phones under their memory limit). The patches file fixes two bugs in the
original game; each entry says why.

### Play on your computer

```bash
python3 web/serve.py                    # then open http://localhost:8007/ and choose your game folder
```

The folder is copied into the browser's private storage once (about 1 GB) and never leaves the computer.
If your executable isn't called `Cricket07.exe`, add `?exe=YourName.exe` to the address. Other options: `?mute`,
`?4x3` (the original 4:3 picture), `?pads=N` (up to 4 controllers), `?touch=1` (on-screen controls).

### Host it for friends

```bash
web/make_site.sh site                   # the page and the build: no game files
```

Upload `site/` to any static host that sends COOP/COEP headers on every file (without them the browser
disables threads). The `_headers` file it writes covers Netlify; for nginx, Caddy and the like, set the two
headers listed in `web/make_site.sh`. Cloudflare Pages caps files at 25 MiB, and `build/recomp.wasm` is
about 30 MB. Each player picks their own game folder on first visit, so the host never serves EA's files.
The build is derived from the game's code, though, so share the link with people who own the game rather
than publicly.

### Online play

Online matches need a small room server where players meet. `server/` is one, as a Cloudflare Worker with
a Durable Object per room (the Workers free plan is enough for a few friends):

```bash
cd server
npx wrangler deploy
npx wrangler secret put NETPLAY_SECRET    # any random string; signs the room tickets
```

Then open the page with `?netplay=https://cricket07-netplay.<your-subdomain>.workers.dev`. One player hosts
a room and shares its code (or invite link); both players need the same build and the same game files.
Inputs travel over a direct WebRTC connection when the players' networks allow it, and through the room
otherwise. For players behind strict networks, a TURN relay helps; see `server/README.md`.

Controls: a game controller, or the keyboard (arrow keys to move, **S** to select, mouse for the cursor),
or on-screen controls on a phone. `RC_NO_AUDIO=1` mutes the native app.

r). Use `RC_NO_AUDIO=1` to mute the native app, or `?mute` on the page. Both players in an online
match need the same build and the same game files.

## Technical notes

- **Memory keeps its original layout.** The executable is loaded at its original address, so every
  pointer and C++ vtable in the game works unchanged. The stack is real too: `call` pushes the actual
  return address.
- **Some code only exists at run time.** The sound engine stores small x86 programs inside its
  sound banks and calls them, so `runtime/interp.c` interprets those.
- **Shaders are converted twice over.** Vertex shaders (`vs.1.1`) run on the CPU. Pixel shaders
  (`ps.1.1`) are translated to WGSL, and the old fixed-function pipeline becomes one large WGSL
  shader.
- **WebGPU can only draw from the page's thread**, but the game renders from its own thread. The
  game thread records each frame as a list of commands, and the page replays them every animation
  frame.
- **WebAssembly has 4 GB of memory for everything**, and the game expects a 4 GB address space of its
  own. Its heap and allocations are squeezed into a 1.25 GB window.
- **WebAssembly stacks have no guard page**, so an overflow silently corrupts memory instead of
  crashing. Game threads get 64 MB stacks.
- **The browser clock counts from 1970.** Timer values that large broke the game's timer code, so
  the clock starts near zero instead.
- **A tight lock/unlock loop in the sound code starved the timer thread** in the browser. Releasing a
  mutex now hands it straight to a waiting thread.
- **Online play is lockstep.** A deterministic scheduler (`host/sched.c`) runs one game thread at a
  time on a virtual clock, so a match depends only on its inputs. Each frame, both games wait for both
  players' inputs and apply them together (`host/netplay.c`), and they compare a hash of the game's
  memory every few frames to catch any drift.
- **Phones have a 2 GB limit per tab.** The game maps its archives (about 575 MB) whole; a lazy view now
  reads each 64 KB piece only when the game first copies from it (`host/core.c`, through a hook on the game's
  memcpy). Safari's garbage collector can also stall after the page writes the game files into storage, so
  the per-frame paths allocate nothing in JavaScript, and the page nudges the collector (`web/loader.js`:
  kickCollector).
- **Widescreen is Hor+.** The cameras get the window's aspect ratio, so the view widens at the same vertical
  field of view; menus keep their 4:3 layout and the HUD's side elements move to the edges
  (`host/widescreen.c`, `host/render_rec.c`). Online matches stay 4:3, since every player must run the same game.
- **Recompiled functions keep registers in local variables** and write them back only around calls,
  which made the build about 30% faster and the WebAssembly file about a quarter smaller.
- **Translation is tested against the original.** `tests/difftest.py` runs each function in a CPU
  emulator and in translated form on the same random inputs, then compares every register and every
  byte of memory touched.

MIT licensed. Cricket 07 and EA Sports are trademarks of Electronic Arts, which is not involved with
this project.
