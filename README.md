# The Sims: Bustin' Out (USA) — Native Windows x64 Static Recompilation

A native Windows x64 static recompilation port of **The Sims: Bustin' Out (USA)** (Original Xbox, Title ID `0x45410043`, XDK `1.0.5558`), built with [`xboxrecomp`](third_party/xboxrecomp).

> **Note:** No copyrighted game assets are included. Supply your own legally dumped Original Xbox ISO (`.iso` / `.xiso.iso`) or extracted disc files.

## Quick Start

1. Double-click **`play.bat`** (or `build\Release\sims_bustin_out_recomp.exe`).
2. The native **Launcher & Settings** window opens automatically:
   - **Game & ISO tab**: Pick your Original Xbox `.iso` / `.xiso.iso` (supports both raw XISO and Redump dual-layer images), verify Title ID `0x45410043`, and click **Extract ISO -> Game Folder** (with live progress bar — your original ISO is never modified), or browse to an already-extracted folder.
   - **Video & Graphics tab**: Choose window resolution (`640x480` up to `2560x1440`), Windowed or Borderless Fullscreen, `4:3 Pillarbox` or `Stretch to Fill`, VSync, D3D11 Hardware Renderer, **GPU Vertex Shaders (HLSL `vs_4_0`)**, **Target Framerate (`30 FPS` Original Xbox / `60 FPS` Unlocked)**, optional projected back-face culling, CPU fallback VSH thread count, and performance logging.
   - **Audio & Controls tab**: Toggle Xbox MCPX Audio (XAudio2), Keyboard Controls, optional Skip Launcher, and view the full Controls Reference.
3. Click **Play The Sims: Bustin' Out** (settings are saved to `sims_launcher.ini`).

### Command-Line Options

| Flag | Description |
|---|---|
| `--help` / `-h` | Show command-line usage help and exit |
| `--launcher` | Force open the Launcher & Settings window (or hold **Shift** while launching) |
| `--play` / `--no-launcher` | Skip the launcher and boot straight into the game using `sims_launcher.ini` |
| `--config <path.ini>` | Load/save launcher settings from a custom INI path |
| `--inspect-iso <file.iso>` | Inspect an Original Xbox ISO (Title Name, Title ID, file count & size) |
| `--extract-iso <file.iso> <out_dir>` | Extract an Original Xbox ISO to `<out_dir>` from the command line |

## Controls

| Xbox Control | Keyboard Binding | XInput Gamepad |
|---|---|---|
| Left Stick (Move / Cursor) | `W` / `A` / `S` / `D` | Left Stick |
| Right Stick (Camera / Zoom) | `I` / `J` / `K` / `L` | Right Stick |
| D-Pad (Menus / Motives) | Arrow Keys | D-Pad |
| A / B / X / Y | `Z` (or `Space`) / `X` / `C` / `V` | A / B / X / Y |
| Left / Right Trigger | `Q` / `E` | LT / RT |
| Black / White Buttons | `1` / `2` | LB / RB |
| Start / Back | `Enter` / `Backspace` | Start / Back |
| Toggle Fullscreen | `Alt + Enter` or `F11` | — |

## Key Features & Verified Status

- **Static Recompilation**: 25,891 / 25,891 functions lifted to C (`src/recomp/gen/`), 0 unresolved ICALLs from boot through live 3D gameplay.
- **Native Direct3D 11 Backend** (`src/nv2a_d3d11_backend.c`, `src/nv2a_vsh_hlsl.c`):
  - **GPU-Side NV2A Vertex Shader → HLSL (`vs_4_0`) Compiler**: Translates 128-bit NV2A microcode (all MAC/ILU ops, `a0` relative addressing, paired pre-write semantics) directly to D3D11 hardware vertex shaders with shader caching, dynamic VB/IB batching, and draw merging (`0.00 ms/f` CPU VSH cost, `1.1–2.4 ms/f` total GPU draw time — a >20× speedup over CPU VSH).
  - **Locked 30 FPS & Optional 60 FPS Unlock**: Runs locked 30.00 FPS with >80% idle headroom by default, or selectable **60 FPS Unlocked** (`RECOMP_MAX_SWAP_INTERVAL=1`) at ~60 FPS with normal 1× in-game clock pacing.
  - Internal 1280×960 HD rendering (Xbox 2×2 FSAA surface) with configurable window resolution, aspect ratio, VSync, and borderless fullscreen (`Alt+Enter` / `F11`).
  - Zero-copy hardware GPU `BC1` / `BC2` / `BC3` (DXT1/3/5) uploads + fast Morton unswizzling for P8 and swizzled formats (including 32×32 P8 portrait chroma-key transparency).
  - Full 8-stage + final-stage NV2A register combiners in HLSL with **stage-1 multi-texturing (`tex1`)**, **specular lighting (`v1`)**, real-time reflections, and accurate stencil/depth shadow handling.
- **Native Audio & Input**: Emulated MCPX APU via **XAudio2** (48 kHz stereo) + **XInput** gamepad & keyboard pad forwarding.
- **Verified Gameplay**: Intro FMVs (`ea_games.xmv`, `sims_intro.xmv`), Title & Main Menu, Save Select (`saves/UDATA/45410043/`), Create-A-Sim, Club Rubb dream sequence, Mom's House (locked 30 FPS / 60 FPS unlocked), Buy Mode, Build Mode, Save → Quit → Reload persistence, and Free Play Mode (`Places to Go: 1 Free Street`).

## Building from Source

### Prerequisites
- Windows 10/11 x64, Visual Studio 2022 (C++ Desktop workload), CMake 3.20+, Python 3.10+ (`pip install capstone`)
- Your own legally dumped Original Xbox ISO or extracted `game/default.xbe` (Title ID `0x45410043`)

### 1. Generate Recompiled C Sources from Your `default.xbe`
Place your extracted disc files (including `default.xbe`) in `game/` and run the included `xboxrecomp` pipeline:

```powershell
python third_party/xboxrecomp/tools/disasm/__main__.py game/default.xbe -o analysis/disasm
python third_party/xboxrecomp/tools/func_id/__main__.py game/default.xbe -d analysis/disasm -o analysis/func_id
python third_party/xboxrecomp/tools/abi_analysis/__main__.py game/default.xbe -d analysis/disasm -o analysis/abi
python third_party/xboxrecomp/tools/recomp/__main__.py game/default.xbe --functions analysis/disasm/functions.json --output src/recomp/gen --split 1000
```

### 2. Configure & Build (Release)

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

## License & Credits

- Port code (`src/`): MIT License
- Vendored [`xboxrecomp`](third_party/xboxrecomp) toolkit: MIT License — Copyright (c) 2026 sp00nz (see `third_party/xboxrecomp/LICENSE` and `third_party/xboxrecomp/NOTICE`)
