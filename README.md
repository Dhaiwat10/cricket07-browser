# Cricket 07 in the browser

EA Sports Cricket 07 (2006, Windows) running in a browser tab, with WebGPU graphics and Web Audio
sound.

The game's executable is translated from x86 machine code to C ahead of time, and the C is compiled
to WebAssembly. Underneath it, this repo reimplements the parts of Windows the game uses: Direct3D 8,
DirectSound, DirectInput and the core system calls.

This repo has no game files and no game code. You need your own copy of Cricket 07.

## From game folder to browser

**1. Start with the game folder.** This is the installed game: `Data/`, `Audio/` and the `.exe`.

**2. Map the executable with Ghidra.** Ghidra finds every function in the `.exe`, and
`ghidra/export.sh` saves the list to `functions.json`.

**3. Translate it to C.** `translator/x86toc.py` reads the `.exe` and `functions.json` and writes one
C function per game function into `out_all/`.

**4. Compile it.** `build.sh` compiles that C together with `runtime/` (the CPU model and the loader)
and `host/` (the Windows layer) into a native Mac app. `build_web.sh` does the same with Emscripten
and produces `recomp.js` and `recomp.wasm`.

**5. Play in the browser.** `web/` holds the page. It asks for your game folder, copies it into the
browser's private storage (the files never leave your machine), and starts the game.

## Using this repo

You need:
- macOS on Apple Silicon
- Python 3 with `capstone` and `pefile`
- [Ghidra](https://ghidra-sre.org) 12
- SDL2 (`brew install sdl2`)
- [wgpu-native](https://github.com/gfx-rs/wgpu-native), unpacked into `third_party/wgpu`
- [Emscripten](https://emscripten.org), for the browser build

```bash
# 1. Export the function list (once)
GHIDRA=~/ghidra_12.1.4_PUBLIC ghidra/export.sh /path/to/game.exe work/functions.json

# 2. Set the executable name and install path for your copy
cp host/profile.h work/profile.h      # then edit it

# 3. Translate and build the native app, then run it
PROFILE=work/profile.h ./build.sh /path/to/game.exe work/functions.json
build/recomp /path/to/game-folder

# 4. Build the browser version and serve it
PROFILE=work/profile.h ./build_web.sh
python3 web/serve.py                  # open http://localhost:8007/ and pick your game folder
```

Controls: arrow keys to move, **S** to select, mouse for the cursor.

Use `RC_NO_AUDIO=1` to mute the native app, or `?mute` on the page.

## What's in here

| Folder | What |
|---|---|
| `ghidra/` | Exports the function list from the executable |
| `translator/` | The x86 → C translator |
| `runtime/` | CPU model, executable loader, function dispatch |
| `host/` | Windows, Direct3D 8 (on WebGPU), sound and input |
| `web/` | The browser page and a local server |
| `tests/` | Checks translated functions against the original, run in an emulator |
| `tools/` | Readers for EA's archive and texture formats |

## License

MIT, for the code in this repo. Cricket 07 and EA Sports are trademarks of Electronic Arts, which is
not involved with this project. Don't publish the game's files or the code generated from them.
