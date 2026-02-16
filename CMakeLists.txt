cmake_minimum_required(VERSION 3.15)
project(fmtuner-sdr VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_SOURCE_DIR}/include)

find_package(OpenSSL REQUIRED)

set(PORTAUDIO_INCLUDE "/opt/homebrew/Cellar/portaudio/19.7.0/include")
set(PORTAUDIO_LIB "/opt/homebrew/Cellar/portaudio/19.7.0/lib")

set(SOURCES
    src/rtl_tcp_client.cpp
    src/fm_demod.cpp
    src/stereo_decoder.cpp
    src/af_post_processor.cpp
    src/cpu_features.cpp
    src/rds_decoder.cpp
    src/xdr_server.cpp
    src/audio_output.cpp
    src/main.cpp
)

add_executable(fmtuner-sdr ${SOURCES})

target_include_directories(fmtuner-sdr PRIVATE 
    ${CMAKE_SOURCE_DIR}/include 
    /opt/homebrew/Cellar/openssl@3/3.6.1/include
    ${PORTAUDIO_INCLUDE}
)

target_link_libraries(fmtuner-sdr pthread ${OPENSSL_CRYPTO_LIBRARY} ${OPENSSL_SSL_LIBRARY} ${PORTAUDIO_LIB}/libportaudio.dylib)

install(TARGETS fmtuner-sdr DESTINATION bin)
