cmake_minimum_required(VERSION 3.15)
project(fm-sdr-tuner VERSION 1.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_SOURCE_DIR}/include)
include(CheckCXXCompilerFlag)
include(GNUInstallDirs)

option(FM_TUNER_ENABLE_X86_AVX2 "Enable AVX2/FMA compile flags on x86 targets" ON)
set(FM_TUNER_DEFAULT_PORTAUDIO OFF)
if(WIN32)
    set(FM_TUNER_DEFAULT_PORTAUDIO OFF)
endif()
option(FM_TUNER_ENABLE_PORTAUDIO "Enable PortAudio for audio output (macOS/Windows)" ${FM_TUNER_DEFAULT_PORTAUDIO})
set(FM_TUNER_ENABLE_ALSA OFF)
if(UNIX AND NOT APPLE)
    set(FM_TUNER_ENABLE_ALSA ON)
endif()

if(APPLE)
    set(FM_TUNER_ENABLE_PORTAUDIO OFF CACHE BOOL "PortAudio is disabled on macOS (native Core Audio backend is used)" FORCE)
endif()
if(WIN32)
    set(FM_TUNER_ENABLE_PORTAUDIO OFF CACHE BOOL "PortAudio is disabled on Windows (native WinMM backend is used)" FORCE)
endif()

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

if(FM_TUNER_ENABLE_PORTAUDIO)
    find_package(PkgConfig QUIET)
    find_package(portaudio CONFIG QUIET)

    set(PORTAUDIO_INCLUDE_DIR "")
    set(PORTAUDIO_LIBRARY "")

    find_path(PORTAUDIO_INCLUDE_DIR
        NAMES portaudio.h
        HINTS /opt/homebrew/include /usr/local/include
    )

    find_library(PORTAUDIO_LIBRARY
        NAMES portaudio portaudio_static portaudio_x64 portaudio_x86
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

    if((NOT PORTAUDIO_INCLUDE_DIR OR NOT PORTAUDIO_LIBRARY)
       AND NOT TARGET PkgConfig::PC_PORTAUDIO
       AND NOT TARGET portaudio
       AND NOT TARGET PortAudio::PortAudio)
        message(FATAL_ERROR "PortAudio not found. Install PortAudio or set PORTAUDIO_INCLUDE_DIR and PORTAUDIO_LIBRARY.")
    endif()
endif()

if(FM_TUNER_ENABLE_ALSA)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PC_ALSA QUIET IMPORTED_TARGET alsa)
    endif()
    if(PC_ALSA_FOUND)
        set(ALSA_INCLUDE_DIRS ${PC_ALSA_INCLUDE_DIRS})
        set(ALSA_LIBRARIES ${PC_ALSA_LIBRARIES})
    else()
        find_path(ALSA_INCLUDE_DIRS NAMES alsa/asoundlib.h)
        find_library(ALSA_LIBRARIES NAMES asound)
    endif()
    if(NOT ALSA_INCLUDE_DIRS OR NOT ALSA_LIBRARIES)
        message(FATAL_ERROR "ALSA not found. Install libasound2-dev or set ALSA_INCLUDE_DIRS and ALSA_LIBRARIES.")
    endif()
endif()

if(APPLE)
    find_library(COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
    find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
    find_library(AUDIOUNIT_FRAMEWORK AudioUnit REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
endif()
if(WIN32)
    find_library(WINMM_LIB winmm REQUIRED)
endif()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_RTLSDR QUIET IMPORTED_TARGET librtlsdr)
    pkg_check_modules(PC_LIQUID QUIET IMPORTED_TARGET liquid)
endif()
if(PC_RTLSDR_FOUND)
    set(RTLSDR_INCLUDE_DIRS ${PC_RTLSDR_INCLUDE_DIRS})
    set(RTLSDR_LIBRARIES ${PC_RTLSDR_LIBRARIES})
else()
    find_path(RTLSDR_INCLUDE_DIRS NAMES rtl-sdr.h)
    find_library(RTLSDR_LIBRARIES NAMES rtlsdr)
endif()
if(NOT RTLSDR_INCLUDE_DIRS OR NOT RTLSDR_LIBRARIES)
    message(FATAL_ERROR "librtlsdr not found. Install librtlsdr.")
endif()

if(PC_LIQUID_FOUND)
    set(LIQUID_INCLUDE_DIRS ${PC_LIQUID_INCLUDE_DIRS})
    set(LIQUID_LIBRARIES ${PC_LIQUID_LIBRARIES})
else()
    find_path(LIQUID_INCLUDE_DIRS NAMES liquid/liquid.h)
    find_library(LIQUID_LIBRARIES NAMES liquid)
endif()
if(NOT LIQUID_INCLUDE_DIRS OR NOT LIQUID_LIBRARIES)
    message(FATAL_ERROR "liquid-dsp not found. Install liquid-dsp (libliquid). Required for DSP and RDS pipelines.")
endif()

set(SOURCES
    src/dsp/liquid_primitives.cpp
    src/dsp/runtime.cpp
    src/rtl_tcp_client.cpp
    src/rtl_sdr_device.cpp
    src/fm_demod.cpp
    src/stereo_decoder.cpp
    src/af_post_processor.cpp
    src/cpu_features.cpp
    src/rds_decoder.cpp
    src/redsea_port/block_sync.cpp
    src/redsea_port/group.cpp
    src/redsea_port/dsp/liquid_wrappers.cpp
    src/redsea_port/dsp/subcarrier.cpp
    src/redsea_port/util/util.cpp
    src/xdr_server.cpp
    src/audio_output.cpp
    src/config.cpp
    src/signal_level.cpp
    src/main.cpp
)

add_executable(fm-sdr-tuner ${SOURCES})
target_compile_definitions(fm-sdr-tuner PRIVATE FM_SDR_TUNER_VERSION="${PROJECT_VERSION}")
target_compile_definitions(fm-sdr-tuner PRIVATE FM_TUNER_HAS_APP_LIQUID_BACKEND)
if(FM_TUNER_ENABLE_PORTAUDIO)
    target_compile_definitions(fm-sdr-tuner PRIVATE FM_TUNER_HAS_PORTAUDIO)
endif()
if(FM_TUNER_ENABLE_ALSA)
    target_compile_definitions(fm-sdr-tuner PRIVATE FM_TUNER_HAS_ALSA)
endif()
if(APPLE)
    target_compile_definitions(fm-sdr-tuner PRIVATE FM_TUNER_HAS_COREAUDIO)
endif()
if(WIN32)
    target_compile_definitions(fm-sdr-tuner PRIVATE FM_TUNER_HAS_WINMM)
endif()
target_compile_definitions(fm-sdr-tuner PRIVATE FM_TUNER_HAS_RTLSDR)

if(FM_TUNER_ENABLE_X86_AVX2)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$")
        if(MSVC)
            target_compile_options(fm-sdr-tuner PRIVATE /arch:AVX2)
            message(STATUS "x86 build: enabling AVX2 via /arch:AVX2")
        else()
            check_cxx_compiler_flag("-mavx2" HAS_MAVX2)
            check_cxx_compiler_flag("-mfma" HAS_MFMA)
            if(HAS_MAVX2 AND HAS_MFMA)
                target_compile_options(fm-sdr-tuner PRIVATE -mavx2 -mfma)
                message(STATUS "x86 build: enabling AVX2/FMA compile flags (-mavx2 -mfma)")
            else()
                message(WARNING "x86 build detected but compiler lacks -mavx2/-mfma; AVX2/FMA paths will not be compiled")
            endif()
        endif()
    else()
        message(STATUS "Non-x86 target (${CMAKE_SYSTEM_PROCESSOR}): skipping AVX2/FMA compile flags")
    endif()
endif()

target_include_directories(fm-sdr-tuner PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
if(FM_TUNER_ENABLE_PORTAUDIO AND PORTAUDIO_INCLUDE_DIR)
    target_include_directories(fm-sdr-tuner PRIVATE ${PORTAUDIO_INCLUDE_DIR})
endif()

target_link_libraries(fm-sdr-tuner PRIVATE
    Threads::Threads
    OpenSSL::Crypto
    OpenSSL::SSL
)
if(FM_TUNER_ENABLE_PORTAUDIO)
    if(TARGET portaudio)
        target_link_libraries(fm-sdr-tuner PRIVATE portaudio)
    elseif(TARGET PortAudio::PortAudio)
        target_link_libraries(fm-sdr-tuner PRIVATE PortAudio::PortAudio)
    elseif(TARGET PkgConfig::PC_PORTAUDIO)
        target_link_libraries(fm-sdr-tuner PRIVATE PkgConfig::PC_PORTAUDIO)
    elseif(PORTAUDIO_LIBRARY)
        target_link_libraries(fm-sdr-tuner PRIVATE ${PORTAUDIO_LIBRARY})
    endif()
endif()
if(FM_TUNER_ENABLE_ALSA)
    if(PC_ALSA_FOUND AND TARGET PkgConfig::PC_ALSA)
        target_link_libraries(fm-sdr-tuner PRIVATE PkgConfig::PC_ALSA)
    else()
        target_include_directories(fm-sdr-tuner PRIVATE ${ALSA_INCLUDE_DIRS})
        target_link_libraries(fm-sdr-tuner PRIVATE ${ALSA_LIBRARIES})
    endif()
endif()
if(PC_RTLSDR_FOUND AND TARGET PkgConfig::PC_RTLSDR)
    target_link_libraries(fm-sdr-tuner PRIVATE PkgConfig::PC_RTLSDR)
else()
    target_include_directories(fm-sdr-tuner PRIVATE ${RTLSDR_INCLUDE_DIRS})
    target_link_libraries(fm-sdr-tuner PRIVATE ${RTLSDR_LIBRARIES})
endif()
if(PC_LIQUID_FOUND AND TARGET PkgConfig::PC_LIQUID)
    target_link_libraries(fm-sdr-tuner PRIVATE PkgConfig::PC_LIQUID)
else()
    target_include_directories(fm-sdr-tuner PRIVATE ${LIQUID_INCLUDE_DIRS})
    target_link_libraries(fm-sdr-tuner PRIVATE ${LIQUID_LIBRARIES})
endif()
if(WIN32)
    target_link_libraries(fm-sdr-tuner PRIVATE ws2_32 ${WINMM_LIB})
endif()
if(APPLE)
    target_link_libraries(fm-sdr-tuner PRIVATE
        ${COREAUDIO_FRAMEWORK}
        ${AUDIOTOOLBOX_FRAMEWORK}
        ${AUDIOUNIT_FRAMEWORK}
        ${COREFOUNDATION_FRAMEWORK}
    )
endif()

install(TARGETS fm-sdr-tuner RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES fm-sdr-tuner.ini DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/fm-sdr-tuner RENAME fm-sdr-tuner.ini.example)
install(FILES README.md LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})

set(CPACK_PACKAGE_NAME "fm-sdr-tuner")
set(CPACK_PACKAGE_VENDOR "Bkram Developments")
set(CPACK_PACKAGE_CONTACT "maintainers@bkram.dev")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "FM broadcast tuner and XDR server for RTL-SDR")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES ON)

if(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "DEB")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Bkram Developments")
    set(CPACK_DEBIAN_PACKAGE_SECTION "hamradio")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libliquid1, librtlsdr0 | librtlsdr2, rtl-sdr")
endif()

include(CPack)

# Tests
enable_testing()
find_package(Catch2 3 QUIET)
if(Catch2_FOUND)
    message(STATUS "Catch2 found, enabling tests")
    add_subdirectory(tests)
else()
    message(STATUS "Catch2 not found, tests disabled. Install with: brew install catch2")
endif()
