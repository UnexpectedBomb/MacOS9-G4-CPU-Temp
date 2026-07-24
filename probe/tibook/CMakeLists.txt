cmake_minimum_required(VERSION 3.13)
project(TiBookProbe C CXX)   # CXX: the CONSOLE runtime (RetroConsole) is C++

# Diagnostic app: on a PowerBook G4 Titanium, tries every avenue we know for a
# CPU/system temperature (on-die TAU, GetCoreProcessorTemperature, i2c sensor
# sweep, device-tree thermal nodes) and prints a verdict. Read-only on all buses.
add_application(TiBookProbe tibook_probe.c CONSOLE)
target_link_libraries(TiBookProbe
    NameRegistryLib DriverServicesLib InterfaceLib)
