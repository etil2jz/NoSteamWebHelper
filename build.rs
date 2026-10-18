fn main() {
    println!("cargo::rerun-if-changed=build.rs");

    // The DLL has no CRT: the loader calls DllMainCRTStartup directly, and
    // every import is resolved through windows-sys's raw-dylib declarations.
    for arg in [
        "-nostdlib",
        "-Wl,--entry=DllMainCRTStartup",
        "-Wl,--gc-sections",
        "-Wl,--no-insert-timestamp",
        // Stripping is left to the linker: on this target, Cargo's strip
        // option only reaches it as --strip-debug, which wins over
        // --strip-all and keeps the COFF symbol table in.
        "-Wl,--strip-all",
        // rustc always passes --unwindlib=none, which -nostdlib leaves unused.
        "-Wno-unused-command-line-argument",
    ] {
        println!("cargo::rustc-cdylib-link-arg={arg}");
    }
}
