cmake_minimum_required(VERSION 3.13)
project(BWProbe C CXX)   # CXX: the CONSOLE runtime (RetroConsole) is C++

# Diagnostic app: on any G3/G4 (aimed at the Power Mac G3 Blue & White), lists
# every i2c controller + every device-tree thermal node and prints a verdict on
# whether ANY temperature is readable from OS 9. Read-only on all buses.
add_application(BWProbe bw_probe.c CONSOLE)
target_link_libraries(BWProbe
    NameRegistryLib DriverServicesLib InterfaceLib)
