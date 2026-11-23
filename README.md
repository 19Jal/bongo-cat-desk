# Bongo Cat Desktop Companion Bot

This repo contains 2 codes, the firmware side for the ESP32 (ESP32-S3 is used here) and the desktop side for the PC (Windows is used here).

## Project Structure
- (Not added yet) 3d-models: Contains the 3D models used in the project.
- firmware: Contains the ESP32 firmware code.
- src: Contains the desktop application code.

## Windows Desktop Application
Currently, the Windows desktop application is a simple console application that listens for key presses and sends them to the ESP32 over USB. The application uses the `hidapi` library to communicate with the ESP32.

## OLED Animation
The Bongo Cat animation header file (firmware/esp32/main/bongo_frame.h) is from https://github.com/younes-makhchan/esp32-bongo-cat-oled

