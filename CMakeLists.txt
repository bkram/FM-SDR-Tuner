cmake_minimum_required(VERSION 3.15)
project(fmtuner-sdr VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_SOURCE_DIR}/include)

find_package(OpenSSL REQUIRED)

set(SOURCES
    src/rtl_tcp_client.cpp
    src/fm_demod.cpp
    src/stereo_decoder.cpp
    src/xdr_server.cpp
    src/audio_output.cpp
    src/main.cpp
)

add_executable(fmtuner-sdr ${SOURCES})

target_include_directories(fmtuner-sdr PRIVATE ${CMAKE_SOURCE_DIR}/include /opt/homebrew/Cellar/openssl@3/3.6.1/include)

target_link_libraries(fmtuner-sdr pthread ${OPENSSL_CRYPTO_LIBRARY} ${OPENSSL_SSL_LIBRARY} ${COREAUDIO} ${AUDIOTOOLBOX})

if(APPLE)
    target_link_libraries(fmtuner-sdr "-framework CoreAudio" "-framework AudioToolbox")
endif()

install(TARGETS fmtuner-sdr DESTINATION bin)
