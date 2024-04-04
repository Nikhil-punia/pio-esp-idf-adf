# Setting up ESP32 IDF with ADF in PlatformIO

## Introduction
Currently PlatformIO doesn't support ADF natively, so there are some steps to get it working.
You can just clone this repo and start using it, but I have also listed the steps below.

Note: currently the espressif32 is seto to 6.5.0 in platformio.ini, this is just as I know this version works.

## To do it yourself
(you can look at the git history of project to see the steps taken too)
1. Create a new Platform IO project
1. Add ESP-ADF submodule with this command: `git submodule add https://github.com/espressif/esp-adf.git esp-adf`
1. Clone all nested submodules with this command: `git submodule update --init --recursive`
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

Notes: 
* You can just move esp-adf/components to /components and don't need full git repository
* You don't need esp-adf/esp-idf (typically old version): `git submodule deinit esp-idf`

##  Optional add example

1. Add to CMakeLists.txt: `set(COMPONENT_SRCS adf_music.mp3)`
1. Add to platformio.ini: `board_build.partitions = partitions.csv`
1. Add data/adf_music.mp3
1. Add partitions.csv
1. Use main.c attached
1. Make sure to "Upload Filesystem Image"