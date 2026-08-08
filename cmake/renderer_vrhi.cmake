if(NOT BUILD_CLIENT OR NOT BUILD_RENDERER_VRHI)
    return()
endif()

include(utils/set_output_dirs)

# The VRHI submodule is intentionally not part of the ioquake3 build graph.
# This target consumes a separately copied set of prebuilt libraries instead.
set(VRHI_ROOT "${SOURCE_DIR}/thirdparty/vrhi" CACHE PATH
    "Root containing copied VRHI artifacts")
set(VRHI_CONFIG "release" CACHE STRING
    "VRHI artifact configuration (debug or release)")
set_property(CACHE VRHI_CONFIG PROPERTY STRINGS debug release)

string(TOLOWER "${VRHI_CONFIG}" VRHI_CONFIG_LOWER)
if(NOT VRHI_CONFIG_LOWER STREQUAL "debug" AND
   NOT VRHI_CONFIG_LOWER STREQUAL "release")
    message(FATAL_ERROR
        "VRHI_CONFIG must be either debug or release (got '${VRHI_CONFIG}')")
endif()

set(VRHI_DEPENDENCY_DIR
    "${VRHI_ROOT}/lib/win_llvm_md_${VRHI_CONFIG_LOWER}" CACHE PATH
    "Directory containing copied VRHI dependency libraries")
set(VRHI_STATIC_LIBRARY
    "${VRHI_ROOT}/build/windows-llvm-md-${VRHI_CONFIG_LOWER}/vrhi_md.lib"
    CACHE FILEPATH "Copied VRHI static library")
set(VRHI_INCLUDE_DIR "${VRHI_ROOT}/include" CACHE PATH
    "Optional copied VRHI include directory")

set(_VRHI_DEPENDENCY_FILES
    "SPIRV-Tools-opt.lib"
    "SPIRV-Tools.lib"
    "nvrhi.lib"
    "nvrhi_vk.lib"
    "rtxmu.lib"
    "vk-bootstrap.lib")

set(_VRHI_MISSING_FILES)
if(NOT EXISTS "${VRHI_STATIC_LIBRARY}")
    list(APPEND _VRHI_MISSING_FILES "${VRHI_STATIC_LIBRARY}")
endif()
foreach(_VRHI_DEPENDENCY_FILE IN LISTS _VRHI_DEPENDENCY_FILES)
    if(NOT EXISTS "${VRHI_DEPENDENCY_DIR}/${_VRHI_DEPENDENCY_FILE}")
        list(APPEND _VRHI_MISSING_FILES
            "${VRHI_DEPENDENCY_DIR}/${_VRHI_DEPENDENCY_FILE}")
    endif()
endforeach()
if(_VRHI_MISSING_FILES)
    string(JOIN "\n  " _VRHI_MISSING_FILES_MESSAGE ${_VRHI_MISSING_FILES})
    message(FATAL_ERROR
        "BUILD_RENDERER_VRHI requires copied VRHI artifacts; missing:\n  "
        "${_VRHI_MISSING_FILES_MESSAGE}\n"
        "Set VRHI_ROOT, VRHI_CONFIG, VRHI_DEPENDENCY_DIR, or "
        "VRHI_STATIC_LIBRARY to their artifact locations.")
endif()

# Keep every prebuilt artifact behind an imported target.  In particular,
# this avoids treating the unbuilt VRHI submodule as a CMake source tree.
function(_vrhi_import_static_target TARGET_NAME LIBRARY_PATH)
    add_library(${TARGET_NAME} STATIC IMPORTED GLOBAL)
    set_target_properties(${TARGET_NAME} PROPERTIES
        IMPORTED_LOCATION "${LIBRARY_PATH}"
        IMPORTED_LINK_INTERFACE_LANGUAGES CXX)
endfunction()

_vrhi_import_static_target(VRHI::vrhi "${VRHI_STATIC_LIBRARY}")
foreach(_VRHI_DEPENDENCY_FILE IN LISTS _VRHI_DEPENDENCY_FILES)
    get_filename_component(_VRHI_DEPENDENCY_NAME
        "${_VRHI_DEPENDENCY_FILE}" NAME_WE)
    string(REPLACE "-" "_" _VRHI_DEPENDENCY_NAME
        "${_VRHI_DEPENDENCY_NAME}")
    _vrhi_import_static_target(
        "VRHI::${_VRHI_DEPENDENCY_NAME}"
        "${VRHI_DEPENDENCY_DIR}/${_VRHI_DEPENDENCY_FILE}")
endforeach()

add_library(VRHI::prebuilt INTERFACE IMPORTED GLOBAL)
set_property(TARGET VRHI::prebuilt PROPERTY INTERFACE_LINK_LIBRARIES
    VRHI::vrhi
    VRHI::nvrhi_vk
    VRHI::nvrhi
    VRHI::vk_bootstrap
    VRHI::rtxmu
    VRHI::SPIRV_Tools_opt
    VRHI::SPIRV_Tools)

set(RENDERER_VRHI_BINARY renderer_vrhi)
add_library(${RENDERER_VRHI_BINARY} SHARED
    ${SOURCE_DIR}/renderervrhi/vrhi_stub.cpp)

target_include_directories(${RENDERER_VRHI_BINARY} PRIVATE
    ${SOURCE_DIR}
    ${SOURCE_DIR}/qcommon)
target_compile_definitions(${RENDERER_VRHI_BINARY} PRIVATE
    USE_RENDERER_DLOPEN)
if(IS_DIRECTORY "${VRHI_INCLUDE_DIR}")
    target_include_directories(${RENDERER_VRHI_BINARY} PRIVATE
        "${VRHI_INCLUDE_DIR}")
endif()
target_link_libraries(${RENDERER_VRHI_BINARY} PRIVATE VRHI::prebuilt)
set_target_properties(${RENDERER_VRHI_BINARY} PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    LINKER_LANGUAGE CXX)

set_output_dirs(${RENDERER_VRHI_BINARY})
