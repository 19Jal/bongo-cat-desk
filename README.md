# Bongo Cat Desktop Companion Bot

Inspired from Bongo Cat Companion software applications found everywhere on the internet (github, Steam, etc.), this project is the hardware version.

The bot is a simple device that listens for key presses on the PC and sends them to the ESP32 over USB. The ESP32 then displays an animation of Bongo Cat on an OLED screen.

This repo contains 2 sides of codes, the firmware side for the ESP32 (ESP32-S3 is used here) and the desktop side for the PC (Windows is used here). 

The Bongo Cat animation header file (firmware/esp32/main/bongo_frame.h) is from https://github.com/younes-makhchan/esp32-bongo-cat-oled. Big thanks to the author for making it available.

## Project Structure
- (Not added yet) 3d-models: Contains the 3D models used in the project.
- firmware: Contains the ESP32 firmware code.
- src: Contains the desktop application code.

## Windows Desktop Application/Script
Currently, the Windows desktop application is a simple python script that listens for key presses and sends them to the ESP32 over USB. The scriptuses the `hidapi` library to communicate with the ESP32.