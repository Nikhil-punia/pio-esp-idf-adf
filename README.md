# Setting up ESP32 IDF with ADF in PlatformIO

## Introduction
Currently PlatformIO doesn't support ADF natively, so there are some steps to get it working.
You can just clone this repo and start using it, but I have also listed the steps below.

Note: currently the espressif32 is seto to 6.6.0 in platformio.ini, this is just as I know this version works.

## To do it yourself
(you can look at the git history of project to see the steps taken too)
1. Create a new Platform IO project
1. Setup platformio.ini settings:
```
board_build.embed_txtfiles = 
  esp-adf/components/dueros_service/duer_profile
    
build_flags =
  -D CONFIG_ESP32_CORVO_DU1906_BOARD
  -I lib/esp_peripherals/

build_unflags = 
  -Wl,--end-group
```
5. Add ADF components to root CMakeLists.txt: `list(APPEND EXTRA_COMPONENT_DIRS "esp-adf/components")`
6. Change setting in sdkconfig.esp32dev: `CONFIG_FREERTOS_ENABLE_BACKWARD_COMPATIBILITY=y`
6. Change setting in sdkconfig.esp32dev: `CONFIG_FREERTOS_UNICORE=y` (This was needed in espressif32 6.6 but not 6.5)


Notes: 
* For adding ESP-ADF , submodule with this command: `git submodule add https://github.com/espressif/esp-adf.git esp-adf`
* Some of the adf components are changed for maintaing the compatiblity with ESP-IDF.
* The current board setup in main.c is for simple esp32s3 module with no codec chip which plays audio through a external audio dac.

## Testing The Example
* You can copy paste any example from the ESP-ADF repo.
* Mostly examples have a option for configuring the parameters in the .c file or in menuconfig.
* Choose the correct board in the menuconfig.
* You can customize the board parameters in the respective board file present in the component folder.
* Compile and Upload and see ESP-ADF in action.
