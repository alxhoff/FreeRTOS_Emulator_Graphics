# FreeRTOS_Emulator_Graphics

Graphics library for the FreeRTOS emulator found [*here*](https://github.com/alxhoff/FreeRTOS-Emulator).

Library is designed to provide a basic interface to drawing simple objects, text and rendering images using the SDL libraries which are inherently single-thread. The library functions around allowing multiple threads to queue draw jobs which are then rendered by a single thread such that SDL does not cry.

Documentation for the library can be found [*here*](https://alxhoff.github.io/FreeRTOS-Emulator/index.html) generated from the emulator's CI.
