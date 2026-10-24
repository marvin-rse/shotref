cmake_minimum_required(VERSION 3.16)
project(shotref_cam CXX)
set(CMAKE_CXX_STANDARD 17)

# Point this at the folder where you unpacked Sony's Camera Remote SDK:
#   cmake -B build -DCRSDK_DIR=$HOME/CrSDK
set(CRSDK_DIR "" CACHE PATH "Path to Sony Camera Remote SDK")
if(NOT CRSDK_DIR)
  message(FATAL_ERROR "Set -DCRSDK_DIR=/path/to/CrSDK (download it from Sony first)")
endif()

include(FetchContent)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)

add_executable(shotref-cam main.cpp)
target_include_directories(shotref-cam PRIVATE ${CRSDK_DIR}/app/CRSDK)
# VERIFY: library folder and names differ per platform (Linux ARMv8 / x86, macOS, Windows).
target_link_directories(shotref-cam PRIVATE ${CRSDK_DIR}/external/crsdk)
target_link_libraries(shotref-cam PRIVATE Cr_Core nlohmann_json::nlohmann_json)
