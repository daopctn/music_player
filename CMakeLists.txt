cmake_minimum_required(VERSION 3.16)
project(jarvis_audio CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# --- libmpv (system, via pkg-config) ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)

# --- FTXUI (fetched at configure time; no system install needed) ---
include(FetchContent)
FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
  GIT_TAG        v5.0.0)
FetchContent_MakeAvailable(ftxui)

find_package(Threads REQUIRED)

add_executable(jarvis
  src/Resolver.cpp
  src/Engine.cpp
  src/Thumbnail.cpp
  src/main.cpp)

target_link_libraries(jarvis
  PRIVATE
    PkgConfig::MPV
    ftxui::screen
    ftxui::dom
    ftxui::component
    Threads::Threads)
