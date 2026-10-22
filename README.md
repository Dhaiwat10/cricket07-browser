# Cricket 07 in the browser

EA Sports Cricket 07 (2006, Windows) running in a browser tab, with game controllers and online 1v1
multiplayer. This repo has no game files and no game code, so you need your own copy of the game.

## How it was made

The game's executable was translated from x86 machine code to C. Ghidra finds the functions, and
`translator/x86toc.py` turns each one into a C function: 31,547 functions, about 1.3 million
instructions. That C is compiled to WebAssembly with Emscripten, or to a native Mac app with clang.

The translated game still expects Windows, so `host/` provides the parts it uses. Direct3D 8 is
rebuilt on WebGPU, DirectSound plays through Web Audio, DirectInput reads browser keyboard and mouse
events and gamepads, and file, thread and timer calls map onto their browser equivalents. In the
browser, the game files live in the browser's private storage, copied in once from a folder you pick.

Online play works without any changes to the game: both players run the same simulation, and only
their controller inputs travel between them. The deterministic scheduler and lockstep netplay were
adapted from an earlier browser port of Rugby 06, which also contributed the controller support.

## Using this repo

You need macOS on Apple Silicon, Python 3 with `capstone` and `pefile`,
[Ghidra](https://ghidra-sre.org) 12, SDL2 (`brew install sdl2`),
[wgpu-native](https://github.com/gfx-rs/wgpu-native) in `third_party/wgpu`, and
[Emscripten](https://emscripten.org).

```bash
# 1. Export the function list from your copy's executable (once)
GHIDRA=~/ghidra_12.1.4_PUBLIC ghidra/export.sh /path/to/game.exe work/functions.json

# 2. Set your executable's name and install path
cp host/profile.h work/profile.h      # then edit it

# 3. Translate and build the native app, then run it
PROFILE=work/profile.h ./build.sh /path/to/game.exe work/functions.json
build/recomp /path/to/game-folder

# 4. Build the browser version and serve it
PROFILE=work/profile.h ./build_web.sh
python3 web/serve.py                  # open http://localhost:8007/ and pick your game folder

# 5. Optional, for online play: deploy the room server (see server/README.md)
cd server && npx wrangler deploy && npx wrangler secret put NETPLAY_SECRET
# then open the page with ?netplay=https://cricket07-netplay.<your-subdomain>.workers.dev
```

Controls: a game controller, or the keyboard (arrow keys to move, **S** to select, mouse for the
cursor). Use `RC_NO_AUDIO=1` to mute the native app, or `?mute` on the page. Both players in an online
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
- **Recompiled functions keep registers in local variables** and write them back only around calls,
  which made the build about 30% faster and the WebAssembly file about a quarter smaller.
- **Translation is tested against the original.** `tests/difftest.py` runs each function in a CPU
  emulator and in translated form on the same random inputs, then compares every register and every
  byte of memory touched.

MIT licensed. Cricket 07 and EA Sports are trademarks of Electronic Arts, which is not involved with
this project.
