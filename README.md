# NoSteamWebHelper
A high-performance utility that disables Steam's CEF (Chromium Embedded Framework) / Steam WebHelper while gaming.

## Aim
This program replaces Steam's removed `-no-browser` command-line parameter. It dynamically toggles the Steam WebHelper based on your activity to free up system resources without losing access to your library.

- **Game running**: Steam WebHelper is disabled (processes terminated and suppressed).
- **No game running**: Steam WebHelper is enabled (interface restored).

## Key Features & Refactoring (2026)
This project has been fully refactored for maximum performance and reliability:
- **Ultra-Lightweight**: Written in pure C with `nostdlib`. The resulting `umpdc.dll` is only a few KB.
- **Modern Build System**: Migrated to **CMake** with support for **LLVM/Clang 22** for state-of-the-art optimizations.
- **Zero-Leak Architecture**: 
  - Uses `Interlocked` atomic operations for thread-safe monitor initialization.
  - Replaced `wtsapi32` dependency with `Toolhelp32` for better compatibility and smaller footprint.
  - Asynchronous registry monitoring using Win32 Events to prevent UI/Hook freezes.
- **Immutable Releases**: Automated CI/CD pipeline via GitHub Actions with cryptographic build provenance (SLSA).

## Suppression Modes
Choose how aggressively to suppress Steam WebHelper processes:

### 🔴 Aggressive Mode (Default)
- **Kills ALL steamwebhelper.exe processes**
- Maximum RAM savings (200-500MB typically)
- Best for: Pure gaming, no Steam overlay needed
- **Steam Input**: ✅ Works (uses native processes, not CEF)
- **Achievements**: ✅ Works (native Steam feature)
- **Steam Overlay**: ❌ Disabled (requires CEF)

### 🟡 Selective Mode
- **Keeps broker and GPU processes, kills renderers**
- Balanced approach: preserves Steam Input UI while saving RAM
- Best for: Users who want Steam Input configuration access
- **Steam Input**: ✅ Works with UI access
- **Achievements**: ✅ Works
- **Steam Overlay**: ⚠️ Partially functional

### 🟢 Disabled Mode
- **No intervention** - Normal Steam behavior
- Best for: Full Steam experience, troubleshooting

## SISR & ViGEmBus Support
Automatic detection of **SISR (Steam Input System Redirector)** and **ViGEmBus**:
- When detected, tray icon turns yellow to indicate headless mode
- Optimized for users running Steam purely for input remapping
- Perfect for non-Steam games using controller rerouting
- **What SISR hooks from Steam**: Uses Steam's native input APIs (not CEF-dependent)
  - Steam Input configuration system
  - Virtual device interfaces (ViGEmBus/viiper)
  - Controller mapping and action sets
  - All work without CEF processes

### Non-Steam Game Scenarios
1. **Direct Steam Input**: Add non-Steam game to Steam library → Steam Input works natively
2. **SISR/ViGEmBus Route**: Steam runs in background → SISR redirects input to any game
3. **Glossi Alternative**: Similar approach using different virtual device layer

In all scenarios, Steam Input configuration requires CEF only briefly during setup, not during gameplay.

## Usage
1. Download the latest release from [GitHub Releases](https://github.com/oraphiio/NoSteamWebHelper/releases/latest).
2. Place `umpdc.dll` in your Steam installation directory (same folder as `steam.exe`).
3. Ensure Steam is fully closed, then launch Steam.
4. The CEF will now toggle automatically when you launch/close a game.

### Mid-Game CEF Toggle
Need to access Steam settings during gameplay?
- Right-click tray icon → Select "Off (Force Enable CEF)"
- Make your changes (controller config, settings, etc.)
- Select "On (Force Suppress)" or close the game to re-enable suppression

> [!TIP]
> - **Tray Icon Colors**: Red (Aggressive), Yellow (Selective/SISR), Green (Disabled)
> - **Right-click tray icon** to change modes or manually toggle CEF
> - **RAM Usage**: Hover over tray icon to see current CEF memory consumption
> - Launch Steam with `-silent` to prevent the CEF from popping up unexpectedly when restored.
> - **Selective Mode**: Keeps Steam Input UI accessible while killing renderer processes
> - **SISR Users**: Automatic detection enables optimal headless mode for input-only usage

### Configuration File Location
The configuration file is stored at:
- **Windows**: `%APPDATA%\NoSteamWebHelper\NoSteamWebHelper.ini`
- Typically: `C:\Users\<YourUsername>\AppData\Roaming\NoSteamWebHelper\NoSteamWebHelper.ini`

## Configuration
Create `%APPDATA%\NoSteamWebHelper\NoSteamWebHelper.ini` to customize behavior:

```ini
[Settings]
Mode=0                    ; 0=Aggressive, 1=Selective, 2=Disabled
EnableTrayTooltip=1       ; Show/hide tray tooltip
ShowRamUsage=1            ; Display RAM usage in tooltip
AutoDetectSiSR=1          ; Auto-detect SISR/ViGEmBus
```

### Mode Descriptions
- **Mode=0 (Aggressive)**: Kill ALL steamwebhelper processes - maximum RAM savings
- **Mode=1 (Selective)**: Keep broker + GPU processes, kill renderers - balanced approach
- **Mode=2 (Disabled)**: No intervention - normal Steam behavior

> [!NOTE]
> **Steam Input does NOT require CEF** - it uses native Steam processes.
> CEF is only needed for:
> - Steam Overlay (Shift+Tab interface)
> - Steam Browser (store, community, profiles)
> - Steam Chat window
> - Web-based store pages
> - **Brief configuration changes** - once config is saved, CEF can be killed again

### What Processes Are Kept in Selective Mode?
- ✅ **Broker process** (no `--type` flag) - main Steam Input coordinator
- ✅ **GPU process** (`--type=gpu-process`) - hardware acceleration for input UI
- ❌ **Renderer processes** (`--type=renderer`) - web content rendering (killed)
- ❌ **Utility processes** (`--type=utility`) - background tasks (killed)

## Build from Source
The project uses **MSYS2** with the **UCRT64** environment.

1. **Install Prerequisites**:
   ```bash
   pacman -Syu mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
   ```

2. **Compile**:
   ```bash
   cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=MinSizeRel
   cmake --build build
   ```
   The optimized `umpdc.dll` will be located in the `build/` directory.

## CI/CD
This project uses GitHub Actions for automated builds. Every tagged release (`v*`) triggers a Clang-optimized build and generates a **Verified Provenance Attestation**, ensuring the binary's integrity.
