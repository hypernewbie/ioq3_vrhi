# GNU style (GCC/Clang) compiler specific settings

if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU" AND NOT CMAKE_C_COMPILER_ID MATCHES "^(Apple)?Clang$")
    return()
endif()

enable_language(ASM)

set(ASM_SOURCES
    ${SOURCE_DIR}/asm/ftola.c
    ${SOURCE_DIR}/asm/snapvector.c
)

if(IOQ3_ENABLE_WARNINGS)
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C,CXX>:-Wall>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:-Wimplicit>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:-Wshadow>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:-Wstrict-prototypes>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:-Wformat=2>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:-Wformat-security>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:-Wstrict-aliasing=2>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:-Wmissing-format-attribute>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:-Wdisabled-optimization>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:-Werror-implicit-function-declaration>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:-Wno-format-zero-length>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:-Wno-format-nonliteral>"
    )
else()
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-w>")
endif()

# There are lots of instances of union based aliasing in the code
# that rely on the compiler not optimising them away, so disable it
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-fno-strict-aliasing>")

# This is necessary to hide all symbols unless explicitly exported
# via the Q_EXPORT macro
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-fvisibility=hidden>")
