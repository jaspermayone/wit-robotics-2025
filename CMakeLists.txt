cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w CACHE STRING "Board type")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(BLUEPAD32_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/bluepad32)

# Use BTstack from Pico SDK
set(BTSTACK_ROOT ${PICO_SDK_PATH}/lib/btstack)

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)

project(monster_book C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# FatFS SD card library
add_subdirectory(no-OS-FatFS/src build_fatfs)

add_executable(${PROJECT_NAME}
        src/main.c
        src/bluetooth/my_platform.c
        src/hardware/motor_controller.c
        src/networking/wifi_sta.c
        src/networking/web_server.c
        src/networking/web_dashboard_page.c
        src/networking/web_motor_test.c
        src/networking/foxglove_ws.c
        src/hardware/motor_pwm_control.c
        src/hardware/motor_omni.c
        src/hardware/motor_bi.c
        src/hardware/imu.c
        src/hardware/sd_storage.c
        src/hardware/dht11.c
        src/hardware/hw_config.c
)

target_include_directories(${PROJECT_NAME} PRIVATE
        src
        src/config
        src/hardware
        src/networking
        src/bluetooth
        ${BLUEPAD32_ROOT}/src/components/bluepad32/include
)

# Needed for btstack_config.h / sdkconfig.h
include_directories(${PROJECT_NAME} src/config)

# Needed for BTstack includes
include_directories(${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/include)
include_directories(${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/include)
include_directories(${BTSTACK_ROOT}/src)

target_link_libraries(${PROJECT_NAME} PUBLIC
        pico_stdlib
        pico_cyw43_arch_lwip_threadsafe_background
        pico_btstack_classic
        pico_btstack_cyw43
        hardware_pwm
        hardware_adc
        bluepad32
        no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
)

add_subdirectory(${BLUEPAD32_ROOT}/src/components/bluepad32 libbluepad32)

# Enable USB serial output
pico_enable_stdio_usb(${PROJECT_NAME} 1)
pico_enable_stdio_uart(${PROJECT_NAME} 0)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(${PROJECT_NAME})

# =============================================================================
# motor_test — standalone USB serial motor test (no Bluetooth / WiFi)
# =============================================================================

add_executable(motor_test
        src/tools/motor_test_main.c
        src/hardware/motor_pwm_control.c
        src/hardware/motor_bi.c
        src/hardware/motor_omni.c
)

target_include_directories(motor_test PRIVATE
        src
        src/config
        src/hardware
)

target_link_libraries(motor_test PUBLIC
        pico_stdlib
        hardware_pwm
)

pico_enable_stdio_usb(motor_test 1)
pico_enable_stdio_uart(motor_test 0)

pico_add_extra_outputs(motor_test)
