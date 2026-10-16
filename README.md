# Cricket 07 in the browser

**EA Sports Cricket 07** (2006, Windows, Direct3D 8) running in a browser tab: the menus, the music
and full matches, with WebGPU graphics and Web Audio sound. It also builds as a native Mac app.

There's no emulator involved. All 31,547 functions in the game's executable were translated from x86
machine code to C ahead of time, about 1.3 million instructions in total. The C is compiled to
WebAssembly, or to native code on a Mac. The Windows parts the game expects (Direct3D 8, DirectSound,
DirectInput, kernel32, user32) are reimplemented underneath it.

> This repository is the recompiler and the runtime. It contains nothing from the game: no assets, no
> game code, no generated code. To run anything you need your own copy of the game.

## From an .exe to a browser tab

```
Cricket07.exe ─▶ Ghidra ─▶ functions.json ─▶ x86toc.py ─▶ 106 C files ─▶ emcc ─▶ recomp.wasm
                                                                         ▲
                                         runtime/ + host/ (Windows, Direct3D 8, audio, input)
```

1. **Find the code.** Ghidra analyses the executable; `ghidra/ExportFunctions.java` exports every
   function's bounds and every switch table it resolved.
2. **Translate it.** `translator/x86toc.py` turns each function into a C function that operates on
   a model of the CPU. It finds about 14,000 more entry points that Ghidra missed, on top of its 17,000: callbacks stored
   in data tables, jumps into the middle of other functions, and code hiding in the gaps between
   functions.
3. **Give it a Windows to run on.** `host/` implements the 234 Windows functions the game imports
   and the DirectX interfaces behind them. Direct3D 8 is the big one: it's reimplemented on WebGPU, shaders included.
4. **Compile it twice.** clang builds a native Mac app; Emscripten builds the WebAssembly one.

## How the translation works

- **One C function per game function**, `void f_0052a3c0(CPU *c)`. Registers, flags, the x87
  floating-point stack and the SSE registers live in a `CPU` struct (`runtime/cpu.h`).
- **Memory keeps its original layout.** The executable is loaded at its original address in a flat
  guest address space, so every pointer, C++ vtable and jump table in the game works unchanged.
- **The stack is real.** `call` pushes the actual return address and `ret` pops it, so code that
  digs its arguments out of the stack sees exactly what it expects.
- **Control flow becomes `goto`** inside a function. Switch tables become C `switch` statements.
  Any other indirect jump looks up its target in an address → function table at run time.
- **x87 maths runs on doubles**, honouring the precision and rounding modes the game sets. Direct3D
  8 switches the FPU to 24-bit precision.
- **Code that only exists at run time** goes to an interpreter. The game's sound engine stores
  little x86 "modules" inside its sound banks, copies them into memory and calls them.
  `runtime/interp.c` runs those (about 3,000 instructions of glue code).

## Direct3D 8 on WebGPU

The game draws with a 2001-era API: fixed-function texture stages, vertex shaders in `vs.1.1`
bytecode and pixel shaders in `ps.1.1`. WebGPU has none of that, so:

- **Fixed-function stages** map onto one large WGSL "uber shader" driven by per-draw state.
- **Vertex shaders** (`host/vshader.c`) run on the CPU, which is cheap at this game's vertex counts.
- **Pixel shaders** (`host/pshader.c`) are translated from `ps.1.1` bytecode to WGSL when the game
  creates them.
- **Render targets and readback** work. The stadium lighting is rendered into a texture that the
  game reads back on the CPU, so the GPU copy has to land in game memory before the game continues.
- **Rendering is split in two** (`host/render_cmd.h`). A recorder on the game thread does all the
  CPU work and emits self-contained commands. An executor owns WebGPU and replays them. In the
  browser WebGPU can only present from the page's thread, so the game thread queues each frame and
  the page replays it on `requestAnimationFrame`.

## Making it run in WebAssembly

The native Mac build worked first. The browser added problems of its own:

- **Memory.** The game expects a 4 GB address space, and wasm32 has 4 GB in total for everything. The
  guest gets a compacted 1.25 GB window instead (`runtime/runtime.h`).
- **Silent stack overflows.** WebAssembly stacks have no guard page, so overflowing one quietly
  corrupts whatever comes next. Recompiled code nests one native frame per game call, so game threads
  get 64 MB stacks and the main game thread gets 256 MB.
- **The clock.** WebAssembly's clock counts from 1970, and `GetTickCount` values that large broke the
  game's timer scheduling. The clock now starts near zero.
- **Starved threads.** The sound code locks and unlocks a mutex in a tight loop, and in the browser
  the timer thread never got a turn. `ReleaseMutex` now hands the mutex directly to a waiting thread.
- **Timed waits that never woke up.** `pthread_cond_timedwait` under Emscripten was replaced with
  polling.
- **Game files** live in the browser's private storage (OPFS). The page copies your game folder
  in once, and the files never leave your machine.
- **Audio never blocks.** The mixer runs on its own clock and writes into a ring buffer in shared
  memory, which an AudioWorklet plays.

## Bugs worth telling

- **The screen washed out to white after the coin toss.** The game clears small rectangles of the screen,
  and WebGPU can only clear a whole texture. Partial clears are now drawn as quads.
- **The players were black.** Their lighting comes from a texture the game renders and then reads
  back on the CPU, which needed render-target readback.
- **The keyboard did nothing in a match.** The game expects DirectInput 7's device type codes, not
  DirectInput 8's, and it ignored the keyboard unless the mouse was enumerated first.
- **Test runs with a hidden window dropped well below 60 fps.** macOS's App Nap was throttling them.
- **A function was never translated.** It sat in padding between two other functions where neither
  Ghidra nor the gap scanner looked; the scanner now also tries starts aligned after `int3` padding.
- **`rep movsd` copied wrong bytes** when source and destination overlapped, and the x87 rounding
  mode wasn't mirrored to the host FPU. Both were found by the differential tester below.

## Testing the translation

`tests/difftest.py` runs each original function under the Unicorn CPU emulator and its translated
version side by side, with the same random registers, arguments and memory. It then compares every
register and every byte either side touched.

| | Functions tested | Identical | Harmless differences* | Real failures |
|---|---|---|---|---|
| Leaf functions | 3,053 | 2,508+ | 19 | ≤16 (9 use unsupported SIMD) |
| With their whole call trees | 14,509 | ~6,466 | 88 | 29 (19 use unsupported SIMD) |

\* NaN bit patterns, x87's wider exponent range at extreme magnitudes, or a divide-by-zero flag quirk
in the emulator. The remaining functions couldn't finish under the emulator: they called Windows,
crashed on random input, or read the CPU's timestamp counter.

## Running it yourself

You need your own copy of the game. This repository doesn't include it, and nothing here downloads
it.

Requirements: macOS on Apple Silicon (for the native build), Python 3 with `capstone`, `pefile` and
`unicorn` (the last only for tests), [Ghidra](https://ghidra-sre.org) 12, SDL2 (`brew install sdl2`),
[wgpu-native](https://github.com/gfx-rs/wgpu-native) in `third_party/wgpu` (or set `WGPU`), and
[Emscripten](https://emscripten.org) for the browser build.

```bash
# 1. Analyse the executable (once)
GHIDRA=~/ghidra_12.1.4_PUBLIC ghidra/export.sh /path/to/game.exe work/functions.json

# 2. Describe the install (executable name, install path, registry values)
cp host/profile.h work/profile.h      # then edit it

# 3. Translate, build and run natively
PROFILE=work/profile.h ./build.sh /path/to/game.exe work/functions.json
build/recomp /path/to/game-folder

# 4. Build for the browser (after step 3) and serve it locally
PROFILE=work/profile.h ./build_web.sh
python3 web/serve.py                  # open http://localhost:8007/ and pick the game folder
```

Keyboard: arrow keys move, **S** selects, and the mouse moves the cursor.

Debugging options (environment variables): `RC_NO_AUDIO=1`, `RC_LOG=1`, `RC_LOG_IMPORTS=1` (every
Windows call), `RC_LOG_D3D=1|2`, `RC_LOG_FPS=1`, `RC_FULLSCREEN=1`, `RC_HEADLESS=1`,
`RC_SCREENSHOTS=120,600` (PNGs in `/tmp`), `RC_INPUT="900:click,1150:key=1f"` (scripted input by
frame), `RC_PACE60=1`, `RC_CAPTURE=N` (save the image after every draw of frame N),
`RC_WATCHDOG=secs`. Page options: `?mute`, `?fps`, `?log`, `?autoplay`, `?exe=name.exe`.

Differential tests: `tests/run_difftest.sh /path/to/game.exe work/functions.json 8 --trials 3`.

## Layout

| Path | What |
|---|---|
| `translator/x86toc.py` | x86 → C translator (Capstone-based) |
| `runtime/` | CPU model, PE loader, dispatch, run-time code interpreter, SIMD helpers |
| `host/kernel32.c`, `user32.c`, `misc.c`, `core.c` | Memory, files, threads, sync objects, window and messages, registry, timers |
| `host/d3d8.c`, `com.c` | Direct3D 8 devices, resources and state |
| `host/render_rec.c`, `render_wgpu.c`, `vshader.c`, `pshader.c` | The renderer: recorder, WebGPU executor, shaders |
| `host/dsound.c`, `dinput.c`, `input.c` | Sound mixing, keyboard and mouse |
| `web/` | The page, its loader, the audio worklet and a local server |
| `ghidra/` | Function export, C++ class recovery from RTTI, naming functions after their assert strings |
| `tests/` | The differential tester |
| `tools/ea_assets.py` | Reads EA's BIG archives, RefPack compression and FSH textures |

## Not done yet

- MMX, 3DNow! and SSE2-integer instructions (the game is told the CPU has none and takes its x87
  paths).
- Fixed-function lighting and fog, cube-map pixel-shader instructions, GDI text.
- C++ exceptions (`RaiseException`).

## License

The code in this repository is MIT licensed (see [LICENSE](LICENSE)). Cricket 07 and EA Sports are
trademarks of Electronic Arts, which has nothing to do with this project. The license doesn't cover
the game or anything generated from it, so don't publish recompiled game code or game files.
