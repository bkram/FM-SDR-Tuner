cmake_minimum_required(VERSION 3.15)

set(FM_TUNER_CATCH2_TARGET Catch2::Catch2WithMain)
set(FM_TUNER_TEST_MAIN_SOURCE)
if(NOT TARGET Catch2::Catch2WithMain)
    if(TARGET Catch2::Catch2)
        set(FM_TUNER_CATCH2_TARGET Catch2::Catch2)
        set(FM_TUNER_TEST_MAIN_SOURCE ${CMAKE_SOURCE_DIR}/tests/catch_main.cpp)
    else()
        message(FATAL_ERROR "Catch2 target not available for tests")
    endif()
endif()

add_executable(test_signal_level test_signal_level.cpp
    ${FM_TUNER_TEST_MAIN_SOURCE}
    ${CMAKE_SOURCE_DIR}/src/signal_level.cpp
    ${CMAKE_SOURCE_DIR}/src/cpu_features.cpp
)
target_include_directories(test_signal_level PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${Catch2_INCLUDE_DIRS}
)
target_link_libraries(test_signal_level PRIVATE
    ${FM_TUNER_CATCH2_TARGET}
    Threads::Threads
)

add_executable(test_config test_config.cpp
    ${FM_TUNER_TEST_MAIN_SOURCE}
    ${CMAKE_SOURCE_DIR}/src/config.cpp
)
target_include_directories(test_config PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${Catch2_INCLUDE_DIRS}
)
target_link_libraries(test_config PRIVATE
    ${FM_TUNER_CATCH2_TARGET}
)

add_executable(test_app_options test_app_options.cpp
    ${FM_TUNER_TEST_MAIN_SOURCE}
    ${CMAKE_SOURCE_DIR}/src/app_options.cpp
    ${CMAKE_SOURCE_DIR}/src/config.cpp
    ${CMAKE_SOURCE_DIR}/src/audio_output.cpp
)
target_include_directories(test_app_options PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${Catch2_INCLUDE_DIRS}
)
target_link_libraries(test_app_options PRIVATE
    ${FM_TUNER_CATCH2_TARGET}
    Threads::Threads
)

add_executable(test_dsp_chain test_dsp_chain.cpp
    ${FM_TUNER_TEST_MAIN_SOURCE}
    ${CMAKE_SOURCE_DIR}/src/dsp/liquid_primitives.cpp
    ${CMAKE_SOURCE_DIR}/src/fm_demod.cpp
    ${CMAKE_SOURCE_DIR}/src/stereo_decoder.cpp
    ${CMAKE_SOURCE_DIR}/src/af_post_processor.cpp
)
target_include_directories(test_dsp_chain PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${Catch2_INCLUDE_DIRS}
)
target_link_libraries(test_dsp_chain PRIVATE
    ${FM_TUNER_CATCH2_TARGET}
    Threads::Threads
)
if(PC_LIQUID_FOUND AND TARGET PkgConfig::PC_LIQUID)
    target_link_libraries(test_dsp_chain PRIVATE PkgConfig::PC_LIQUID)
else()
    target_include_directories(test_dsp_chain PRIVATE ${LIQUID_INCLUDE_DIRS})
    target_link_libraries(test_dsp_chain PRIVATE ${LIQUID_LIBRARIES})
endif()

add_executable(test_runtime test_runtime.cpp
    ${FM_TUNER_TEST_MAIN_SOURCE}
    ${CMAKE_SOURCE_DIR}/src/dsp/liquid_primitives.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/runtime.cpp
)
target_include_directories(test_runtime PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${Catch2_INCLUDE_DIRS}
)
target_link_libraries(test_runtime PRIVATE
    ${FM_TUNER_CATCH2_TARGET}
    Threads::Threads
)
if(PC_LIQUID_FOUND AND TARGET PkgConfig::PC_LIQUID)
    target_link_libraries(test_runtime PRIVATE PkgConfig::PC_LIQUID)
else()
    target_include_directories(test_runtime PRIVATE ${LIQUID_INCLUDE_DIRS})
    target_link_libraries(test_runtime PRIVATE ${LIQUID_LIBRARIES})
endif()

add_executable(test_audio_output test_audio_output.cpp
    ${FM_TUNER_TEST_MAIN_SOURCE}
    ${CMAKE_SOURCE_DIR}/src/audio_output.cpp
)
target_include_directories(test_audio_output PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${Catch2_INCLUDE_DIRS}
)
target_link_libraries(test_audio_output PRIVATE
    ${FM_TUNER_CATCH2_TARGET}
    Threads::Threads
)

add_executable(test_rtl_sdr_stub test_rtl_sdr_stub.cpp
    ${FM_TUNER_TEST_MAIN_SOURCE}
    ${CMAKE_SOURCE_DIR}/src/rtl_sdr_device.cpp
)
target_include_directories(test_rtl_sdr_stub PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${Catch2_INCLUDE_DIRS}
)
target_link_libraries(test_rtl_sdr_stub PRIVATE
    ${FM_TUNER_CATCH2_TARGET}
    Threads::Threads
)

add_executable(test_protocol test_protocol.cpp
    ${FM_TUNER_TEST_MAIN_SOURCE}
    ${CMAKE_SOURCE_DIR}/src/xdr_server.cpp
    ${CMAKE_SOURCE_DIR}/src/rtl_tcp_client.cpp
)
target_include_directories(test_protocol PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${Catch2_INCLUDE_DIRS}
)
target_link_libraries(test_protocol PRIVATE
    ${FM_TUNER_CATCH2_TARGET}
    Threads::Threads
    OpenSSL::Crypto
)
if(WIN32)
    target_link_libraries(test_protocol PRIVATE ws2_32)
endif()

add_executable(test_xdr_unit test_xdr_unit.cpp
    ${FM_TUNER_TEST_MAIN_SOURCE}
    ${CMAKE_SOURCE_DIR}/src/xdr_server.cpp
)
target_include_directories(test_xdr_unit PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${Catch2_INCLUDE_DIRS}
)
target_link_libraries(test_xdr_unit PRIVATE
    ${FM_TUNER_CATCH2_TARGET}
    Threads::Threads
    OpenSSL::Crypto
)
if(WIN32)
    target_link_libraries(test_xdr_unit PRIVATE ws2_32)
endif()

add_executable(test_xdr_facade test_xdr_facade.cpp
    ${FM_TUNER_TEST_MAIN_SOURCE}
    ${CMAKE_SOURCE_DIR}/src/xdr_facade.cpp
    ${CMAKE_SOURCE_DIR}/src/xdr_server.cpp
)
target_include_directories(test_xdr_facade PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${Catch2_INCLUDE_DIRS}
)
target_link_libraries(test_xdr_facade PRIVATE
    ${FM_TUNER_CATCH2_TARGET}
    Threads::Threads
    OpenSSL::Crypto
)
if(WIN32)
    target_link_libraries(test_xdr_facade PRIVATE ws2_32)
endif()

add_executable(test_scan_engine test_scan_engine.cpp
    ${FM_TUNER_TEST_MAIN_SOURCE}
    ${CMAKE_SOURCE_DIR}/src/scan_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/signal_level.cpp
    ${CMAKE_SOURCE_DIR}/src/cpu_features.cpp
    ${CMAKE_SOURCE_DIR}/src/xdr_server.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/liquid_primitives.cpp
)
target_include_directories(test_scan_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${Catch2_INCLUDE_DIRS}
)
target_link_libraries(test_scan_engine PRIVATE
    ${FM_TUNER_CATCH2_TARGET}
    Threads::Threads
    OpenSSL::Crypto
)
if(PC_LIQUID_FOUND AND TARGET PkgConfig::PC_LIQUID)
    target_link_libraries(test_scan_engine PRIVATE PkgConfig::PC_LIQUID)
else()
    target_include_directories(test_scan_engine PRIVATE ${LIQUID_INCLUDE_DIRS})
    target_link_libraries(test_scan_engine PRIVATE ${LIQUID_LIBRARIES})
endif()
if(WIN32)
    target_link_libraries(test_scan_engine PRIVATE ws2_32)
endif()

add_test(NAME signal_level COMMAND test_signal_level)
add_test(NAME config COMMAND test_config)
add_test(NAME app_options COMMAND test_app_options)
add_test(NAME dsp_chain COMMAND test_dsp_chain)
add_test(NAME runtime COMMAND test_runtime)
add_test(NAME audio_output COMMAND test_audio_output)
add_test(NAME rtl_sdr_stub COMMAND test_rtl_sdr_stub)
add_test(NAME protocol COMMAND test_protocol)
add_test(NAME xdr_unit COMMAND test_xdr_unit)
add_test(NAME xdr_facade COMMAND test_xdr_facade)
add_test(NAME scan_engine COMMAND $<TARGET_FILE:test_scan_engine>)
