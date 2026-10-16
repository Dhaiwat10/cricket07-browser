# win32-web-recomp

Runs a 2000s Windows game in the browser by **static recompilation**: every function in the game's
x86 executable is translated to C ahead of time, and the C is compiled to native code (macOS) or to
WebAssembly. A small Windows layer stands in for the operating system. Direct3D 8 is reimplemented on
WebGPU, and DirectSound and DirectInput on the Web Audio API and the browser's input events.

It was developed against a 2006 Direct3D 8 sports game (EA Sports Cricket 07), which plays through its
menus into full matches, with audio, in Chrome and natively on Apple Silicon.

**This repository contains no game code or data.** You need your own copy of a game. The recompiled
code is generated on your machine from your executable and must not be redistributed: it is a
translation of the game's code. Executables whose code is encrypted by copy protection (SafeDisc,
SecuROM and similar) can't be translated, and this project neither includes nor supports removing
such protection.

## How it works

```
game.exe ──Ghidra──▶ functions.json ──translator/x86toc.py──▶ out_all/*.c ──clang / emcc──▶ native or wasm
                                                                              + runtime/  (CPU model, loader)
                                                                              + host/     (Windows, D3D8, audio, input)
```

### The translator (`translator/x86toc.py`)

- **One C function per guest function**, `void f_XXXXXXXX(CPU *c)`. Registers, flags, the x87 stack
  and XMM registers live in a `CPU` struct (`runtime/cpu.h`).
- **Guest memory is a flat address space** (`g_mem`) with the PE image at its original base, so every
  pointer, vtable and jump table in the original keeps working unchanged.
- **The guest stack is real**: `call` pushes the actual return address and `ret N` pops it, so code
  that reads its arguments through `ESP` behaves exactly.
- **Branches inside a function become `goto`s**; switch tables become C `switch` statements. Other
  indirect calls and jumps go through a runtime address → function table.
- **Disassembly is recursive descent** from each entry, using Ghidra's function bounds only as limits
  (its body ranges can split instructions). Extra entry points are discovered from code addresses
  stored in data, jumps into the middle of other functions (MSVC's `strcpy` jumps into `strcat`;
  unwind funclets jump into their parents) and gaps between known functions that decode as code.
- **Switch tables** come from Ghidra; tables it missed (`jmp [reg*4 + table]`) are recovered by
  reading entries while they point into the function.
- **Windows imports**: import-table slots hold magic addresses (`0xF0000000+`) that dispatch to host
  implementations; an unimplemented import traps with its name.
- **x87** runs on host doubles, honouring precision control (Direct3D 8 sets 24-bit precision) and the
  rounding mode.
- **CPUID** reports a plain Pentium-class CPU, so games choose their x87 code paths rather than
  MMX/3DNow!/SSE ones.

### The runtime (`runtime/`)

The PE loader, dispatch, traps and SIMD helpers. It also has a small **x86 interpreter**
(`runtime/interp.c`) for code that only exists at run time: the case-study game's sound engine ships
x86 "modules" inside its sound banks, relocates them into the heap and calls them. Regions are
registered when the game marks memory executable (`VirtualProtect`).

### The Windows layer (`host/`)

| File | What |
|---|---|
| `kernel32.c` | Memory, files mapped onto the game folder (case-insensitive), threads on real host threads, sync objects, TLS, locale |
| `user32.c` | A virtual window and message queue that calls the game's window procedure |
| `misc.c` | GDI stubs, a small virtual registry, multimedia timers, DirectX entry points |
| `core.c` | Guest heap allocator, handles, path mapping |
| `d3d8.c`, `com.c` | Direct3D 8 devices, resources and state, as COM objects in guest memory |
| `render_rec.c` | The recorder: primitive expansion, vertex shaders, texture conversion, uniforms → commands |
| `render_wgpu.c` | The executor: every WebGPU object, a fixed-function "uber shader", render targets, readback |
| `vshader.c`, `pshader.c` | vs.1.1 vertex shaders interpreted on the CPU; ps.1.1 pixel shaders translated to WGSL |
| `dsound.c` | DirectSound buffers mixed on a clock-paced thread into CoreAudio or a shared ring for an AudioWorklet |
| `dinput.c`, `input.c` | DirectInput 7 keyboard and mouse |
| `profile.h` | Per-game settings: executable name, install path, registry values |

**Rendering is split in two** (`host/render_cmd.h`). The recorder runs on the game thread and emits
self-contained commands; the executor owns WebGPU. Natively each command runs at once. In the browser,
WebGPU only presents from a thread that returns to its event loop, so the page thread replays queued
command chunks once per animation frame. Readbacks (games that render to a texture and read it back on
the CPU) pause the queue until the copy is mapped.

### What it took to run in WebAssembly

- **Compact guest memory.** wasm32 has a single 4 GiB memory, so the guest gets a 1.25 GiB window
  with the heap and `VirtualAlloc` ranges compacted (`runtime/runtime.h`).
- **Thread stacks.** Wasm stacks in linear memory have no guard page, so an overflow silently
  corrupts memory. Recompiled frames are large, so game threads get 64 MB stacks (the main game thread
  256 MB).
- **A clock near zero.** The epoch-based clock made `GetTickCount`/`timeGetTime` so large that the
  game's timer code lost precision, so `host_seconds()` counts from process start.
- **Fair Win32 mutexes.** `ReleaseMutex` hands the mutex straight to a waiting thread. Without that,
  sound code that locks and unlocks in a tight loop starves the timer thread.
- **Polling instead of timed waits** where `pthread_cond_timedwait` never woke up under Emscripten.
- `lock`-prefixed instructions run under a global lock; file reads loop on short reads.
- **Files** live in the Origin Private File System (WASMFS's OPFS backend, mounted at `/opfs`). The
  page copies the player's chosen game folder there once, and it never leaves the computer.
- **Audio** never blocks: the mixer is paced by the clock and writes into a ring in shared memory that
  an AudioWorklet drains.

## Testing

`tests/difftest.py` is a **differential tester**. It runs each original function under Unicorn and
the recompiled C side by side, on identical random registers, arguments and memory, and compares every
register and every byte either side touched. On the case-study executable (~17,500 functions):

| Suite | Tested | Pass | Benign* | Fail |
|---|---|---|---|---|
| Leaf functions | 3,053 | 2,508+ | 19 | ≤16 (9 unsupported SIMD) |
| Functions with their whole call trees | 14,509 | ~6,466 | 88 | 29 (19 unsupported SIMD) |

\* NaN bit patterns, x87 exponent range at extreme magnitudes, or QEMU's divide-by-zero flag quirk.
The remaining functions couldn't finish under Unicorn (Windows calls, crashes on random data, `rdtsc`).

## Building

Requirements: macOS on Apple Silicon (for the native build), Python 3 with `capstone`, `pefile` and
`unicorn` (the last only for tests), [Ghidra](https://ghidra-sre.org) 12, SDL2 (`brew install sdl2`),
[wgpu-native](https://github.com/gfx-rs/wgpu-native) unpacked in `third_party/wgpu` (or set `WGPU`), and
[Emscripten](https://emscripten.org) for the browser build.

```bash
# 1. Analyse your executable and export its functions (once)
GHIDRA=~/ghidra_12.1.4_PUBLIC ghidra/export.sh /path/to/game.exe work/functions.json

# 2. Describe the game (exe name, install path, registry values): copy host/profile.h and edit it
cp host/profile.h work/profile.h

# 3. Translate and build natively, then run with the game folder
PROFILE=work/profile.h ./build.sh /path/to/game.exe work/functions.json
build/recomp /path/to/game-folder

# 4. Browser build (after step 3, which generates out_all/)
PROFILE=work/profile.h ./build_web.sh
python3 web/serve.py          # http://localhost:8007/ - pick the game folder on the page
```

Options (environment variables): `RC_NO_AUDIO=1`, `RC_LOG=1` (host log), `RC_LOG_IMPORTS=1` (every
Windows call), `RC_LOG_D3D=1|2`, `RC_LOG_FPS=1`, `RC_FULLSCREEN=1`, `RC_HEADLESS=1`,
`RC_SCREENSHOTS=120,600` (PNG to `/tmp/rc_frame_N.png`), `RC_INPUT="900:click,1150:key=1f"`
(scripted input by frame; DIK codes in hex), `RC_PACE60=1` (fixed 60 Hz for repeatable scripts),
`RC_CAPTURE=N` (log every draw of frame N and save the image after each), `RC_WATCHDOG=secs` (print
guest call stacks periodically). Page options: `?mute`, `?fps`, `?log`, `?autoplay`, `?exe=name.exe`.

Tests: `tests/run_difftest.sh /path/to/game.exe work/functions.json 8 --trials 3 [--allow-calls]`.

`tools/` has helpers for inspecting PE files and the archive/texture formats of mid-2000s EA games.

## Known gaps

- MMX, 3DNow! and SSE2-integer instructions are unsupported (CPUID reports none of them).
- NaN bit patterns differ from x87; x87's wider exponent range isn't modelled.
- Direct3D 8: no fixed-function lighting or fog yet, no cube-map pixel-shader ops; GDI text is stubbed.
- C++ exceptions (`RaiseException`) aren't implemented.
- Only what the case-study game needed is implemented; another game will hit unimplemented imports
  (they trap with the name, so you know where to start).

## License

MIT, see [LICENSE](LICENSE). This covers the code in this repository only, not any game or anything
generated from one.
