if(NOT BUILD_CLIENT OR NOT BUILD_RENDERER_VRHI)
    return()
endif()

include(utils/set_output_dirs)

# This renderer currently consumes the Windows Vulkan artifacts produced by
# the pinned VRHI build. Keep the optional target out of all vanilla paths.
if(NOT WIN32)
    message(FATAL_ERROR
        "BUILD_RENDERER_VRHI is currently supported on Windows only")
endif()

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
if(VRHI_CONFIG_LOWER STREQUAL "debug")
    set(_VRHI_STATIC_LIBRARY_NAME "vrhi_mdd.lib")
else()
    set(_VRHI_STATIC_LIBRARY_NAME "vrhi_md.lib")
endif()
set(VRHI_STATIC_LIBRARY
    "${VRHI_ROOT}/build/windows-llvm-md-${VRHI_CONFIG_LOWER}/${_VRHI_STATIC_LIBRARY_NAME}"
    CACHE FILEPATH "Copied VRHI static library")
set(VRHI_INCLUDE_DIR "${VRHI_ROOT}/include" CACHE PATH
    "Optional copied VRHI include directory")

# vrhi_md.lib is a prebuilt static library. Its public link dependencies are
# therefore resolved here, on the renderer target only; Vulkan remains an
# optional dependency of BUILD_RENDERER_VRHI rather than of the main build.
find_package(Vulkan REQUIRED)

get_filename_component(_VRHI_VULKAN_LIB_DIR
    "${Vulkan_LIBRARY}" DIRECTORY)
if(DEFINED VRHI_SLANG_INCLUDE_DIR AND VRHI_SLANG_INCLUDE_DIR)
    set(_VRHI_SLANG_INCLUDE_DIR "${VRHI_SLANG_INCLUDE_DIR}")
else()
    find_path(_VRHI_SLANG_INCLUDE_DIR slang.h
        HINTS
            "$ENV{VULKAN_SDK}/Include"
            "$ENV{VULKAN_SDK}/include"
            "${Vulkan_INCLUDE_DIR}/.."
        PATH_SUFFIXES slang)
endif()
if(DEFINED VRHI_SLANG_LIBRARY AND VRHI_SLANG_LIBRARY)
    set(_VRHI_SLANG_LIBRARY "${VRHI_SLANG_LIBRARY}")
else()
    find_library(_VRHI_SLANG_LIBRARY NAMES slang slang.lib
        HINTS
            "$ENV{VULKAN_SDK}/Lib"
            "$ENV{VULKAN_SDK}/lib"
            "${_VRHI_VULKAN_LIB_DIR}")
endif()
set(VRHI_SLANG_INCLUDE_DIR "${_VRHI_SLANG_INCLUDE_DIR}" CACHE PATH
    "Vulkan SDK Slang include directory used by renderer_vrhi" FORCE)
set(VRHI_SLANG_LIBRARY "${_VRHI_SLANG_LIBRARY}" CACHE FILEPATH
    "Vulkan SDK slang.lib used by renderer_vrhi" FORCE)
if(NOT VRHI_SLANG_INCLUDE_DIR OR NOT VRHI_SLANG_LIBRARY)
    message(FATAL_ERROR
        "BUILD_RENDERER_VRHI requires slang.h and slang.lib from the Vulkan SDK; "
        "set VRHI_SLANG_INCLUDE_DIR and VRHI_SLANG_LIBRARY explicitly")
endif()

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
# The VRHI DLL is a separate renderer target, so it cannot inherit the
# common renderer's JPEG/puff object sources. Reuse the already-discovered
# JPEG variables when a GL renderer configured them, and discover the same
# dependency locally when VRHI is built by itself.
set(_VRHI_JPEG_SOURCES ${RENDERER_LIBRARY_SOURCES})
set(_VRHI_JPEG_INCLUDE_DIRS ${JPEG_INCLUDE_DIRS})
set(_VRHI_JPEG_DEFINITIONS ${JPEG_DEFINITIONS})
set(_VRHI_JPEG_LIBRARIES ${JPEG_LIBRARIES})
if(USE_INTERNAL_JPEG AND NOT _VRHI_JPEG_SOURCES)
    set(_VRHI_INTERNAL_JPEG_DIR ${SOURCE_DIR}/thirdparty/jpeg-${JPEG_VERSION})
    file(GLOB_RECURSE _VRHI_JPEG_SOURCES ${_VRHI_INTERNAL_JPEG_DIR}/j*.c)
    include(utils/find_include_dirs)
    find_include_dirs(_VRHI_JPEG_INCLUDE_DIRS ${_VRHI_INTERNAL_JPEG_DIR})
    list(APPEND _VRHI_JPEG_DEFINITIONS USE_INTERNAL_JPEG)
elseif(NOT USE_INTERNAL_JPEG AND NOT _VRHI_JPEG_LIBRARIES)
    find_package(JPEG REQUIRED)
    set(_VRHI_JPEG_INCLUDE_DIRS ${JPEG_INCLUDE_DIRS})
    set(_VRHI_JPEG_LIBRARIES ${JPEG_LIBRARIES})
endif()

add_library(${RENDERER_VRHI_BINARY} SHARED
    ${SOURCE_DIR}/renderervrhi/vrhi_stub.cpp
    ${SOURCE_DIR}/renderervrhi/vrhi_image_decode.cpp
    ${SOURCE_DIR}/renderercommon/puff.c
    ${_VRHI_JPEG_SOURCES})

if(NOT IS_DIRECTORY "${VRHI_INCLUDE_DIR}")
    message(FATAL_ERROR
        "BUILD_RENDERER_VRHI requires VRHI_INCLUDE_DIR containing vrhi.h and nvrhi")
endif()

if(NOT SDL2_LIBRARIES)
    message(FATAL_ERROR
        "BUILD_RENDERER_VRHI requires the SDL2 target configured by the client build")
endif()

target_include_directories(${RENDERER_VRHI_BINARY} PRIVATE
    ${SOURCE_DIR}
    ${SOURCE_DIR}/qcommon
    "${VRHI_INCLUDE_DIR}"
    "${VRHI_ROOT}"
    "${VRHI_SLANG_INCLUDE_DIR}"
    ${SDL2_INCLUDE_DIRS}
    ${Vulkan_INCLUDE_DIRS}
    "${Vulkan_INCLUDE_DIR}"
    ${_VRHI_JPEG_INCLUDE_DIRS})
target_compile_definitions(${RENDERER_VRHI_BINARY} PRIVATE
    USE_RENDERER_DLOPEN
    ${RENDERER_DEFINITIONS}
    ${_VRHI_JPEG_DEFINITIONS})
if(RENDERER_COMPILE_OPTIONS)
    target_compile_options(${RENDERER_VRHI_BINARY} PRIVATE
        ${RENDERER_COMPILE_OPTIONS})
endif()

# Keep all renderer-only dependencies target-local. The copied VRHI archives
# are imported above; these are the SDK/system/SDL libraries they expose.
target_link_libraries(${RENDERER_VRHI_BINARY} PRIVATE
    VRHI::prebuilt
    Vulkan::Vulkan
    "${VRHI_SLANG_LIBRARY}"
    ${_VRHI_JPEG_LIBRARIES}
    ${SDL2_LIBRARIES}
    dxgi
    shlwapi)
set_target_properties(${RENDERER_VRHI_BINARY} PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    LINKER_LANGUAGE CXX)

set_output_dirs(${RENDERER_VRHI_BINARY})
