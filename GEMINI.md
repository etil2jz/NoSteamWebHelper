# NoSteamWebHelper - Instructions & Context

## Project Overview
NoSteamWebHelper is a high-performance Win32 utility (C-based) that dynamically disables Steam's Chromium Embedded Framework (CEF) / WebHelper when a game is running. It aims to replace the removed `-no-browser` flag, freeing system resources while keeping Steam functional.

### Architecture
- **Language**: Pure C (Win32 API), optimized for binary size (`nostdlib`).
- **Core Component**: `umpdc.dll`, designed to be loaded via DLL side-loading/injection (placed next to `steam.exe`).
- **Mechanics**:
  - **Event Hooking**: Uses `SetWinEventHook` to detect Steam window creations.
  - **Registry Monitoring**: Asynchronously monitors `HKCU\SOFTWARE\Valve\Steam\RunningAppID` to detect game state changes.
  - **Process Management**: Uses `Toolhelp32` snapshotting to identify and terminate `steamwebhelper.exe` processes spawned by Steam.
  - **Thread Safety**: Employs `Interlocked` atomic operations for race-condition-free initialization of the monitoring thread.
  - **UI**: Provides a minimalist system tray icon for manual toggling and status feedback.

## Building and Pipeline
The project uses a modern CMake-based build system optimized for size and performance.

### Build Requirements
- **MSYS2** (UCRT64 environment)
- **LLVM/Clang** (preferred) or GCC
- **CMake** & **Ninja**

### Compilation Commands
```bash
# Configuration for maximum size optimization
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=MinSizeRel

# Build
cmake --build build
```
Output binary: `build/umpdc.dll`

### CI/CD Pipeline
- **Platform**: GitHub Actions (`.github/workflows/release.yml`)
- **Features**:
  - **Validation**: Compiles the project on every Pull Request to `main`.
  - **Deployment**: On version tags (`v*`), performs a production build using **LLVM/Clang 22**.
  - **Immutability & Trust**: Generates **GitHub Artifact Attestations** (SLSA) for every release, ensuring cryptographic proof of provenance.

## Development Conventions
- **Minimalism**: Avoid C Standard Library (`nostdlib`). Use direct Win32/NT APIs.
- **Size Optimization**: Use aggressive compiler flags (`-Oz`, `-fno-stack-protector`, `-fno-asynchronous-unwind-tables`).
- **Nomenclature**: Use descriptive constants (defined in `Library.c`) instead of magic strings or numbers.
- **Safety**: Always close Handles and use atomic operations when managing shared state between the Main thread and the Registry Monitor thread.
- **Provenance**: All production binaries must be built through the CI pipeline to maintain "Immutable" status.