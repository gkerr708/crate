cmake_minimum_required(VERSION 3.20)
project(crate LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- FTXUI (the TUI library): downloaded the first time you configure ---
include(FetchContent)
FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
    GIT_TAG v6.1.9
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ftxui)

# --- TagLib (reads title/artist/album tags): installed with pacman ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(TAGLIB REQUIRED IMPORTED_TARGET taglib)

add_executable(crate src/main.cpp src/library.cpp src/player.cpp src/download.cpp src/playlist.cpp src/manifest.cpp src/progress.cpp)
target_compile_options(crate PRIVATE -Wall -Wextra)
target_link_libraries(crate PRIVATE
    ftxui::screen ftxui::dom ftxui::component
    PkgConfig::TAGLIB
)
