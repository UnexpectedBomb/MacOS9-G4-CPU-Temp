cmake_minimum_required(VERSION 3.13)
project(CPUTempProbe C CXX)   # CXX: the CONSOLE runtime (RetroConsole) is C++

# Diagnostic application: reads the DS1775 CPU temperature from user space and
# prints the result to a RetroConsole window. CONSOLE = stdio window.
add_application(CPUTempProbe cpu_temp_probe.c CONSOLE)
target_link_libraries(CPUTempProbe
    NameRegistryLib DriverServicesLib InterfaceLib)
