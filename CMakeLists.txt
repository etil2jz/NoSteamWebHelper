cmake_minimum_required(VERSION 3.10)
project(NoSteamWebHelper C)

add_library(umpdc SHARED src/Library.c)
set_target_properties(umpdc PROPERTIES PREFIX "")

# The Win32 API is called through its explicit -W entry points, but UNICODE
# still has to be defined so that macros such as IDI_APPLICATION resolve to
# their wide variant. -municode used to provide it as a side effect, while
# really selecting a Unicode CRT startup that a -nostdlib DLL never uses.
target_compile_definitions(umpdc PRIVATE UNICODE _UNICODE)

# Extreme size optimization & no stdlib
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(umpdc PRIVATE 
        -Oz 
        -Wall
        -Wextra
        -fno-stack-protector
        -fno-ident
        -fno-asynchronous-unwind-tables
        -ffunction-sections 
        -fdata-sections
    )
    target_link_options(umpdc PRIVATE 
        -nostdlib
        -Wl,--entry=DllMainCRTStartup
        -Wl,--gc-sections
        -Wl,--exclude-all-symbols
        -Wl,--no-insert-timestamp
        -s
    )
    target_link_libraries(umpdc PRIVATE kernel32 user32 advapi32 shell32)
elseif(MSVC)
    target_compile_options(umpdc PRIVATE /O1 /W4 /GS- /Zl)
    target_link_options(umpdc PRIVATE 
        /NODEFAULTLIB 
        /ENTRY:DllMainCRTStartup
        /OPT:REF
        /OPT:ICF
    )
    target_link_libraries(umpdc PRIVATE kernel32.lib user32.lib advapi32.lib shell32.lib)
endif()
