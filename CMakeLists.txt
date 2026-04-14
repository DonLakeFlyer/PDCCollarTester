cmake_minimum_required(VERSION 3.15)
project(PDCCollarTester CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build only libairspy from the airspyone_host submodule (skip tools, etc.)
add_subdirectory(lib/airspyone_host/libairspy)

# Static libusb for zero runtime dependencies
find_library(LIBUSB_STATIC libusb-1.0.a PATHS /opt/homebrew/lib /usr/local/lib)
if(NOT LIBUSB_STATIC)
    message(FATAL_ERROR "Static libusb not found. Install with: brew install libusb")
endif()

add_executable(collar_tester main.cpp)

target_include_directories(collar_tester PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/airspyone_host/libairspy/src
)

# Link static airspy + static libusb + macOS frameworks
target_link_libraries(collar_tester
    airspy-static
    ${LIBUSB_STATIC}
    "-framework IOKit"
    "-framework CoreFoundation"
    "-framework Security"
)
