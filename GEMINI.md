# NoSteamWebHelper

## Project Overview
NoSteamWebHelper is a (now deprecated) C-based utility designed to disable Steam's CEF (Chromium Embedded Framework) / Steam WebHelper when a game is running. It was created to replace the removed `-no-browser` command-line parameter in Steam. The project builds a dynamic link library (`umpdc.dll`) that checks if an app is running and toggles the CEF accordingly, allowing Steam to be accessible while freeing system resources during gameplay.

## Architecture & Mechanics
- **Language**: C (Win32 API)
- **Main Source**: `src/Library.c`
- **Output**: `bin/umpdc.dll`
- **Core Logic**:
  - Injected via DLL load order (placing `umpdc.dll` next to `steam.exe`).
  - Uses `SetWinEventHook` to detect `vguiPopupWindow` creations.
  - Monitors the `HKEY_CURRENT_USER\SOFTWARE\Valve\Steam\RunningAppID` registry key to check if an app is running.
  - Terminates `steamwebhelper.exe` processes spawned by the current process using `WTSEnumerateProcessesW` and `NtQueryInformationProcess`.
  - Creates a system tray icon (`Shell_NotifyIconW`) with a context menu to manually toggle the behavior by overriding the `RunningAppID` registry value.

## Building and Running
The project is built using MSYS2 and the MinGW-w64 toolchain.

1. **Prerequisites**: Install MSYS2 and update it. Then, install the UCRT64 GCC toolchain:
   ```bash
   pacman -Syu --noconfirm
   pacman -Syu mingw-w64-ucrt-x86_64-gcc --noconfirm
   ```
2. **Build Command**: Start the MSYS2 `UCRT64` environment and run the provided build script from the project root:
   ```cmd
   Build.cmd
   ```
   This script compiles `src/Library.c` and outputs the resulting binary to `bin/umpdc.dll`.
3. **Usage/Installation**:
   - Place the compiled `umpdc.dll` in your Steam installation directory where `steam.exe` is located.
   - Make sure Steam is fully closed and launch a new instance of Steam.
   - Start an app to see the CEF toggled automatically.
   - *(Note: Launch Steam with the `-silent` flag to prevent the CEF from automatically showing when restored.)*

## Development Conventions
- **Toolchain & Flags**: The `Build.cmd` script is heavily optimized for binary size. It uses size optimization (`-Oz`), gc-sections (`-Wl,--gc-sections`), removes symbols (`-Wl,--exclude-all-symbols`, `-s`), and omits the standard library (`-nostdlib`).
- **Coding Style**: The C codebase uses native Windows API calls (e.g., `ntdll`, `wtsapi32`, `kernel32`, `user32`, `advapi32`, `shell32`) and relies on Windows-specific types. It employs succinct inline type casting and compound literals (e.g., `&((DWORD){sizeof(BOOL)})`) characteristic of low-level systems programming.