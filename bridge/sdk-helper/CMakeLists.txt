cmake_minimum_required(VERSION 3.16)
if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.1" CACHE STRING "Minimum macOS version")
endif()
project(shotref_cam CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CRSDK_DIR = the unpacked RemoteCli folder from Sony's SDK download
# (it contains app/CRSDK/*.h and external/crsdk/*). Example:
#   cmake -B build -DCRSDK_DIR=$HOME/CrSDK/RemoteCli
set(CRSDK_DIR "" CACHE PATH "Unpacked RemoteCli folder of Sony's Camera Remote SDK")
if(NOT CRSDK_DIR OR NOT EXISTS "${CRSDK_DIR}/app/CRSDK/CameraRemote_SDK.h")
  message(FATAL_ERROR "Set -DCRSDK_DIR to the unpacked RemoteCli folder of the SDK (must contain app/CRSDK/CameraRemote_SDK.h)")
endif()

include(FetchContent)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
                     URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
FetchContent_MakeAvailable(json)

add_executable(shotref-cam main.cpp)
target_include_directories(shotref-cam PRIVATE "${CRSDK_DIR}/app/CRSDK")
find_library(CR_CORE Cr_Core HINTS "${CRSDK_DIR}/external/crsdk" REQUIRED NO_DEFAULT_PATH)
target_link_libraries(shotref-cam PRIVATE ${CR_CORE} nlohmann_json::nlohmann_json)

if(APPLE)
  set_target_properties(shotref-cam PROPERTIES BUILD_RPATH "@executable_path")
elseif(UNIX)
  set_target_properties(shotref-cam PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
  target_compile_options(shotref-cam PRIVATE -fsigned-char -fstack-protector-all)
endif()
if(WIN32)
  target_compile_definitions(shotref-cam PRIVATE UNICODE _UNICODE)
endif()

# The SDK loads its adapters (USB / PTP-IP / SSH) from next to the executable.
add_custom_command(TARGET shotref-cam POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CRSDK_DIR}/external/crsdk" "$<TARGET_FILE_DIR:shotref-cam>")
