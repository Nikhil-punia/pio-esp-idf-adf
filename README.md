# Setting up ESP32 IDF with ADF in PlatformIO

## Introduction
Currently PlatformIO doesn't support ADF natively, so there are some steps to get it working.
You can just clone this repo and start using it, but I have also listed the steps below.

Note: currently the espressif32 is seto to 6.6.0 in platformio.ini, this is just as I know this version works.

## To do it yourself
(you can look at the git history of project to see the steps taken too)
1. Create a new Platform IO project
1. Add ESP-ADF submodule with this command: `git submodule add https://github.com/espressif/esp-adf.git esp-adf`
1. 