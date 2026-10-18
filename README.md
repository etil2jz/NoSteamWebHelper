# NoSteamWebHelper
A high-performance utility that disables Steam's CEF (Chromium Embedded Framework) / Steam WebHelper while gaming.

## Aim
This program replaces Steam's removed `-no-browser` command-line parameter. It dynamically toggles the Steam WebHelper based on your activity to free up system resources without losing access to your library.

- **Game running**: Steam WebHelper is disabled (processes terminated and suppressed).
- **No game running**: Steam WebHelper is enabled (interface restored).

## Key Features
- **Ultra-Lightweight**: Written in `no_std` Rust with no C runtime. The resulting `umpdc.dll` is around 10 KB and imports nothing but `kernel32`, `user32`, `gdi32`, `advapi32` and `shell32`.
- **Panic-Free by Construction**: Every panic path is optimized out of the release build, and CI checks the import table to keep it that way.
- **Zero-Leak Architecture**:
  - Handles and registry keys are closed by RAII guards, whichever way a thread exits.
  - Atomic operations for thread-safe monitor initialization.
  - `Toolhelp32` snapshots to find and terminate the whole WebHelper process tree.
  - Asynchronous registry monitoring using Win32 Events to prevent UI/Hook freezes.
- **Immutable Releases**: Automated CI/CD pipeline via GitHub Actions with cryptographic build provenance (SLSA).

## Usage
1. Download the latest release from [GitHub Releases](https://github.com/etil2jz/NoSteamWebHelper/releases/latest).
2. Place `umpdc.dll` in your Steam installation directory (same folder as `steam.exe`).
3. Ensure Steam is fully closed, then launch Steam.
4. The CEF will now toggle automatically when you launch/close a game.

> [!TIP]
> - Right-click the tray icon and untick **Toggle** to keep the WebHelper running during games; unticking it mid-game restores it at once. The choice is kept in `HKCU\Software\NoSteamWebHelper` across Steam restarts.
> - Hover the tray icon to see how much RAM the suspended WebHelper is saving.
> - Launch Steam with `-silent` to prevent the CEF from popping up unexpectedly when restored.

## Build from Source
The project targets `x86_64-pc-windows-gnullvm` and links with LLVM/Clang from **MSYS2**'s **CLANG64** environment.

1. **Install Prerequisites**:
   - [rustup](https://rustup.rs/): the toolchain, target and components pinned in `rust-toolchain.toml` are installed on first build. The build script runs on the host, so either install with `rustup-init --default-host x86_64-pc-windows-gnullvm`, or keep the default MSVC host and install the Visual Studio Build Tools for its linker.
   - Clang, from an MSYS2 shell, with `C:\msys64\clang64\bin` added to `PATH`:
     ```bash
     pacman -Syu mingw-w64-clang-x86_64-clang
     ```

2. **Compile**:
   ```bash
   cargo build --release
   ```
   The optimized `umpdc.dll` will be located in `target/x86_64-pc-windows-gnullvm/release/`.

## CI/CD
This project uses GitHub Actions for automated builds. Every pull request is checked with `rustfmt` and `clippy`, and every tagged release (`v*`) triggers an optimized build and generates a **Verified Provenance Attestation**, ensuring the binary's integrity.
