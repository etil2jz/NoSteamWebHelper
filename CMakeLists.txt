cmake_minimum_required(VERSION 3.10)
project(NoSteamWebHelper C)

add_library(umpdc SHARED src/Library.c)
set_target_properties(umpdc PROPERTIES PREFIX "")

# Extreme size optimization & no stdlib
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(umpdc PRIVATE 
        -Oz 
        -municode 
        -fno-stack-protector
        -fno-ident
        -fno-asynchronous-unwind-tables
        -ffunction-sections 
        -fdata-sections
    )
    target_link_options(umpdc PRIVATE 
        -nostdlib
        -Wl,--gc-sections
        -Wl,--exclude-all-symbols
        -Wl,--no-insert-timestamp
        -s
    )
    target_link_libraries(umpdc PRIVATE ntdll kernel32 user32 advapi32 shell32)
elseif(MSVC)
    target_compile_options(umpdc PRIVATE /O1 /GS- /Zl)
    target_link_options(umpdc PRIVATE 
        /NODEFAULTLIB 
        /ENTRY:DllMainCRTStartup
        /OPT:REF
        /OPT:ICF
        /MERGE:.rdata=.text
    )
    target_link_libraries(umpdc PRIVATE ntdll.lib kernel32.lib user32.lib advapi32.lib shell32.lib)
endif()
