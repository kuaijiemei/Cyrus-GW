cmake_minimum_required(VERSION 3.20)
project(cyrus-gateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# For clangd / IDE: generates build/compile_commands.json when you configure with CMake.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

file(GLOB_RECURSE CYRUS_GATEWAY_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(cyrus-gateway ${CYRUS_GATEWAY_SOURCES})
target_include_directories(cyrus-gateway PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(MSVC)
  target_compile_options(cyrus-gateway PRIVATE /W4 /permissive-)
else()
  target_compile_options(cyrus-gateway PRIVATE -Wall -Wextra -Wpedantic -fcoroutines)
endif()

find_package(Threads REQUIRED)
target_link_libraries(cyrus-gateway PRIVATE Threads::Threads)
