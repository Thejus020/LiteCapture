cmake_minimum_required(VERSION 3.20)
project(LiteCapture VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enforce 64-bit builds
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "LiteCapture requires 64-bit architecture (x64). 32-bit builds are unsupported.")
endif()

# Static runtime linking (MultiThreaded / MT) so the executable is 100% standalone without needing MSVC redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")

# Global definitions
add_compile_definitions(
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    LITECAPTURE_VERSION="0.1.0"
)

# MSVC specific compiler options
if(MSVC)
    add_compile_options(
        /W4
        /permissive-
        /utf-8
        /EHsc
        /volatile:iso
        /Zc:inline
        /Zc:throwingNew
    )
    # Enable multi-processor compilation
    add_compile_options(/MP)
endif()

# Source files
set(SOURCES
    src/main.cpp
    src/core/Logger.cpp
    src/app/Config.cpp
    src/app/App.cpp
    src/app/RecordingController.cpp
    src/ui/MainWindow.cpp
    src/graphics/D3D11DeviceManager.cpp
    src/capture/WgcCaptureEngine.cpp
    src/encoder/MfVideoEncoder.cpp
    src/audio/WasapiAudioCapture.cpp
    resources/LiteCapture.manifest
)

# Header files
set(HEADERS
    include/core/Common.hpp
    include/core/Logger.hpp
    include/core/Result.hpp
    include/app/Config.hpp
    include/app/App.hpp
    include/app/RecordingController.hpp
    include/ui/MainWindow.hpp
    include/graphics/AdapterInfo.hpp
    include/graphics/D3D11DeviceManager.hpp
    include/capture/CaptureTypes.hpp
    include/capture/ICaptureEngine.hpp
    include/capture/WgcCaptureEngine.hpp
    include/encoder/EncoderTypes.hpp
    include/encoder/IVideoEncoder.hpp
    include/encoder/MfVideoEncoder.hpp
    include/audio/AudioTypes.hpp
    include/audio/WasapiAudioCapture.hpp
)

add_executable(LiteCapture WIN32 ${SOURCES} ${HEADERS})

target_include_directories(LiteCapture PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Windows system libraries
target_link_libraries(LiteCapture PRIVATE
    comctl32
    dwmapi
    shcore
    shlwapi
    d3d11
    dxgi
    mfplat
    mfreadwrite
    mfuuid
    windowsapp
    ole32
    avrt
    winmm
    shell32
)

# Set IDE folder properties
set_target_properties(LiteCapture PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)
