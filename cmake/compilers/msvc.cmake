# MSVC compiler specific settings

if(NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC" AND
   NOT CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
    return()
endif()

include(utils/arch)

if(ARCH MATCHES "x86" OR ARCH MATCHES "x86_64")
    enable_language(ASM_MASM)

    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        # clang-cl cannot assemble the MASM snapvector/ftola sources with
        # GNU-style inline assembly available in the corresponding C files.
        set(ASM_SOURCES
            ${SOURCE_DIR}/asm/snapvector.c
            ${SOURCE_DIR}/asm/ftola.c
        )
    else()
        set(ASM_SOURCES
            ${SOURCE_DIR}/asm/snapvector.asm
            ${SOURCE_DIR}/asm/ftola.asm
        )
    endif()
endif()

if(ARCH MATCHES "x86_64")
    list(APPEND ASM_SOURCES ${SOURCE_DIR}/asm/vm_x86_64.asm)
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        set_source_files_properties(
            ${SOURCE_DIR}/asm/vm_x86_64.asm
            PROPERTIES COMPILE_DEFINITIONS "idx64")
    else()
        set_source_files_properties(
            ${ASM_SOURCES}
            PROPERTIES COMPILE_DEFINITIONS "idx64")
    endif()
endif()

# Baseline warnings
if(IOQ3_ENABLE_WARNINGS)
    add_compile_options("$<$<COMPILE_LANGUAGE:C>:/W4>")
else()
    add_compile_options("$<$<COMPILE_LANGUAGE:C>:/W0>")
endif()

# C4267: 'var' : conversion from 'size_t' to 'type', possible loss of data
# There are way too many of these to realistically deal with them
add_compile_options("$<$<COMPILE_LANGUAGE:C>:/wd4267>")

# C4206: nonstandard extension used: translation unit is empty
add_compile_options("$<$<COMPILE_LANGUAGE:C>:/wd4206>")

# C4324: 'struct': structure was padded due to alignment specifier
add_compile_options("$<$<COMPILE_LANGUAGE:C>:/wd4324>")

# C4200: nonstandard extension used: zero-sized array in struct/union
add_compile_options("$<$<COMPILE_LANGUAGE:C>:/wd4200>")

# MSVC doesn't understand __inline__, which libjpeg uses
add_compile_definitions(__inline__=inline)

# It's unlikely that we'll move to the _s variants, so stop the warning
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

# The sockets platform abstraction layer necessarily uses deprecated APIs
add_compile_definitions(_WINSOCK_DEPRECATED_NO_WARNINGS)
