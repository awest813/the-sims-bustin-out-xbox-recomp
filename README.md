# The Sims: Bustin' Out (USA) — Native Windows x64 Static Recompilation

A native Windows x64 static recompilation port of **The Sims: Bustin' Out (USA)** (Original Xbox, Title ID `0x45410043`, XDK `1.0.5558`), built with [`xboxrecomp`](third_party/xboxrecomp).

> **Note:** No copyrighted game assets are included. Supply your own legally dumped Original Xbox ISO (`.iso` / `.xiso.iso`) or extracted disc files.

## 1-Click Setup (Recommended)

No command-line knowledge needed:

1. **Install prerequisites** (Windows 10/11 x64 — one-time install if you don't already have them):
   ```powershell
   winget install Python.Python.3.12 Kitware.CMake
   winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
   ```
   *(Already have Visual Studio 2019/2022 with C++ and Python 3.10+? Skip straight to step 2.)*
2. **Download or clone this repository**, then **double-click `setup.bat`**:
   - Automatically verifies Python, CMake, and Visual Studio C++, and installs `capstone` via `pip` if missing.
   - Auto-detects a nearby Xbox `.iso` / `.xiso.iso` or opens a **Windows File Explorer picker** so you can select your legally dumped *The Sims: Bustin' Out (USA)* disc image (supports both raw XISO and Redump images — your original ISO is never modified).
   - Automatically extracts the ISO into `game/`, statically recompiles `default.xbe` into `src/recomp/gen/`, and compiles the native Release executable (`build\Release\sims_bustin_out_recomp.exe`).
   - Offers to launch the game immediately when finished!
3. **Play anytime**: Double-click **`play.bat`** (if you haven't run setup yet, `play.bat` will offer to run `setup.bat` for you).

### Native Launcher & Settings GUI

Launching **`play.bat`** (or `build\Release\sims_bustin_out_recomp.exe`) opens the native 3-tab **Launcher & Settings** window:
- **Game & ISO tab**: Inspect/extract another Xbox ISO with a live progress bar, switch game/save directories, or open your Saves folder.
- **Video & Graphics tab**: Choose window resolution (`640x480` up to `2560x1440`), Windowed or Borderless Fullscreen, `4:3 Pillarbox` or `Stretch to Fill`, VSync, D3D11 Hardware Renderer, **GPU Vertex Shaders (HLSL `vs_4_0`)**, **Target Framerate (`30 FPS` Original Xbox / `60 FPS` Unlocked)**, optional projected back-face culling, CPU fallback VSH thread count, and performance logging.
- **Audio & Controls tab**: Toggle Xbox MCPX Audio (XAudio2; cleanly mutes host output when unchecked), Keyboard Controls, optional Skip Launcher, and view the full Controls Reference.
- Click **Play The Sims: Bustin' Out** (all settings persist in `sims_launcher.ini`).

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

## Manual Build (Advanced)

If you prefer running each step manually instead of double-clicking `setup.bat`:

### 1. Extract Your ISO & Generate Recompiled C Sources
Extract your `.iso` into `game/` (using `python -m tools.xiso unpack <file.iso> -o game` with `PYTHONPATH=third_party\xboxrecomp`) and run:

```powershell
.\scripts\recomp.ps1
```

Or invoke all four pipeline stages directly:

```powershell
$env:PYTHONPATH = "$PWD\third_party\xboxrecomp"
python -m tools.disasm game\default.xbe -o analysis\disasm
python -m tools.func_id game\default.xbe --functions analysis\disasm\functions.json --strings analysis\disasm\strings.json --xrefs analysis\disasm\xrefs.json -o analysis\func_id
python -m tools.abi_analysis game\default.xbe --disasm-dir analysis\disasm --func-id-dir analysis\func_id --output-dir analysis\abi
python -m tools.recomp game\default.xbe --game-name "The Sims Bustin Out" --all --split 250 --gen-dir src\recomp\gen --disasm-dir analysis\disasm --func-id-dir analysis\func_id --abi-dir analysis\abi --exclude-manual src\recomp_manual.c
```

### 2. Configure & Build (Release)

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

## Troubleshooting

- **Windows SmartScreen / PowerShell execution policy**: Always double-click `setup.bat` rather than `scripts\setup.ps1` directly — `setup.bat` launches PowerShell with `-ExecutionPolicy Bypass` automatically.
- **"Visual Studio C++ Build Tools not found"**: Open Visual Studio Installer -> Modify -> check **Desktop development with C++**, then re-run `setup.bat`.
- **"Python not found"**: Re-run the Python installer and check **Add python.exe to PATH**, or run `winget install Python.Python.3.12`.
- **Re-open Launcher after enabling "Skip launcher"**: Hold **Shift** while double-clicking `play.bat`, or run `play.bat --launcher`.
- **Want 60 FPS?** Open Launcher -> **Video & Graphics** -> set **Target Framerate** to `60 FPS (Unlocked 60 Hz)`.
- **Setup CLI flags**: Run `setup.bat -CheckOnly` to verify prerequisites without modifying anything, `setup.bat -ForceRecomp` to re-lift `default.xbe`, or `setup.bat -ForceBuild` to force a fresh Release build.

## Project Layout

| Path | Description |
|---|---|
| `src/main.c` | Native Win32 entry point, CLI switches, Xbox memory/kernel/APU/OHCI/D3D11 bring-up, VEH crash diagnostics |
| `src/recomp_manual.c` | Manual overrides (`D3D_BlockOnTime` volatile semaphore wait, `MU_Init`, XAPI/CRT helpers) |
| `src/nv2a_d3d11_backend.c/.h` | Native Direct3D 11 NV2A backend (combiners, stage-1 multitex, BC1–BC3, RT/DSV/tex LRU caches, borderless fullscreen) |
| `src/nv2a_vsh_hlsl.c/.h` | GPU-side 128-bit NV2A vertex shader microcode → HLSL (`vs_4_0`) translator |
| `src/launcher_ui.c/.h`, `src/launcher_config.c/.h` | Native Win32 3-tab Launcher, Settings UI, and `sims_launcher.ini` persistence |
| `src/xiso_extract.c/.h` | Read-only Original Xbox XISO / Redump ISO inspector & extractor |
| `setup.bat`, `scripts/setup.ps1` | 1-click interactive setup wizard (prereq check, ISO picker/extractor, recomp, Release build) |
| `scripts/recomp.ps1` | Standalone helper script to lift `game/default.xbe` into `src/recomp/gen/` |
| `third_party/xboxrecomp/` | Vendored MIT-licensed `xboxrecomp` runtime & Python recompilation toolkit |

## License & Credits

- Port code (`src/`, `scripts/`): [MIT License](LICENSE)
- Vendored [`xboxrecomp`](third_party/xboxrecomp) toolkit: MIT License — Copyright (c) 2026 sp00nz (see `third_party/xboxrecomp/LICENSE` and `third_party/xboxrecomp/NOTICE`)
