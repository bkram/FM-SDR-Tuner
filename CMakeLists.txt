cmake_minimum_required(VERSION 3.15)
project(fm-tuner-sdr VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_SOURCE_DIR}/include)
include(CheckCXXCompilerFlag)

option(FM_TUNER_ENABLE_X86_AVX2 "Enable AVX2/FMA compile flags on x86 targets" ON)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)

set(PORTAUDIO_INCLUDE_DIR "")
set(PORTAUDIO_LIBRARY "")

find_path(PORTAUDIO_INCLUDE_DIR
    NAMES portaudio.h
    HINTS /opt/homebrew/include /usr/local/include
)

find_library(PORTAUDIO_LIBRARY
    NAMES portaudio portaudio_static
    HINTS /opt/homebrew/lib /usr/local/lib
)

if((NOT PORTAUDIO_INCLUDE_DIR OR NOT PORTAUDIO_LIBRARY) AND PkgConfig_FOUND)
    pkg_check_modules(PC_PORTAUDIO QUIET IMPORTED_TARGET portaudio-2.0)
    if(PC_PORTAUDIO_FOUND)
        if(NOT PORTAUDIO_INCLUDE_DIR)
            set(PORTAUDIO_INCLUDE_DIR ${PC_PORTAUDIO_INCLUDE_DIRS})
        endif()
        if(NOT PORTAUDIO_LIBRARY)
            if(DEFINED pkgcfg_lib_PC_PORTAUDIO_portaudio)
                set(PORTAUDIO_LIBRARY ${pkgcfg_lib_PC_PORTAUDIO_portaudio})
            endif()
        endif()
    endif()
endif()

if(APPLE AND (NOT PORTAUDIO_INCLUDE_DIR OR NOT PORTAUDIO_LIBRARY))
    execute_process(
        COMMAND brew --prefix portaudio
        OUTPUT_VARIABLE PORTAUDIO_BREW_PREFIX
        RESULT_VARIABLE PORTAUDIO_BREW_RESULT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(PORTAUDIO_BREW_RESULT EQUAL 0 AND PORTAUDIO_BREW_PREFIX)
        if(NOT PORTAUDIO_INCLUDE_DIR)
            find_path(PORTAUDIO_INCLUDE_DIR
                NAMES portaudio.h
                HINTS ${PORTAUDIO_BREW_PREFIX}/include
            )
        endif()
        if(NOT PORTAUDIO_LIBRARY)
            find_library(PORTAUDIO_LIBRARY
                NAMES portaudio portaudio_static
                HINTS ${PORTAUDIO_BREW_PREFIX}/lib
            )
        endif()
    endif()
endif()

if((NOT PORTAUDIO_INCLUDE_DIR OR NOT PORTAUDIO_LIBRARY) AND NOT TARGET PkgConfig::PC_PORTAUDIO)
    message(FATAL_ERROR "PortAudio not found. Install PortAudio or set PORTAUDIO_INCLUDE_DIR and PORTAUDIO_LIBRARY.")
endif()

set(SOURCES
    src/rtl_tcp_client.cpp
    src/fm_demod.cpp
    src/stereo_decoder.cpp
    src/af_post_processor.cpp
    src/cpu_features.cpp
    src/rds_decoder.cpp
    src/xdr_server.cpp
    src/audio_output.cpp
    src/config.cpp
    src/main.cpp
)

add_executable(fm-tuner-sdr ${SOURCES})
target_compile_definitions(fm-tuner-sdr PRIVATE FM_TUNER_SDR_VERSION="${PROJECT_VERSION}")

if(FM_TUNER_ENABLE_X86_AVX2)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$")
        if(MSVC)
            target_compile_options(fm-tuner-sdr PRIVATE /arch:AVX2)
            message(STATUS "x86 build: enabling AVX2 via /arch:AVX2")
        else()
            check_cxx_compiler_flag("-mavx2" HAS_MAVX2)
            check_cxx_compiler_flag("-mfma" HAS_MFMA)
            if(HAS_MAVX2 AND HAS_MFMA)
                target_compile_options(fm-tuner-sdr PRIVATE -mavx2 -mfma)
                message(STATUS "x86 build: enabling AVX2/FMA compile flags (-mavx2 -mfma)")
            else()
                message(WARNING "x86 build detected but compiler lacks -mavx2/-mfma; AVX2/FMA paths will not be compiled")
            endif()
        endif()
    else()
        message(STATUS "Non-x86 target (${CMAKE_SYSTEM_PROCESSOR}): skipping AVX2/FMA compile flags")
    endif()
endif()

target_include_directories(fm-tuner-sdr PRIVATE 
    ${CMAKE_SOURCE_DIR}/include
)
if(PORTAUDIO_INCLUDE_DIR)
    target_include_directories(fm-tuner-sdr PRIVATE ${PORTAUDIO_INCLUDE_DIR})
endif()

target_link_libraries(fm-tuner-sdr PRIVATE
    Threads::Threads
    OpenSSL::Crypto
    OpenSSL::SSL
)
if(TARGET PkgConfig::PC_PORTAUDIO)
    target_link_libraries(fm-tuner-sdr PRIVATE PkgConfig::PC_PORTAUDIO)
elseif(PORTAUDIO_LIBRARY)
    target_link_libraries(fm-tuner-sdr PRIVATE ${PORTAUDIO_LIBRARY})
endif()

install(TARGETS fm-tuner-sdr DESTINATION bin)
