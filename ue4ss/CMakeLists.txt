# Minefactor UE4SS C++ mod, cross-compiled on Linux (xwin + clang-cl). See tools/build_ue.sh.
# RE-UE4SS must be the exact commit (272ce2f8) the installed UE4SS.dll was built from; we also build
# and deploy our own UE4SS.dll from it (a clang-cl mod crashes under an MSVC-built core).
cmake_minimum_required(VERSION 3.20)
project(Minefactor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/RE-UE4SS/CMakeLists.txt")
    message(FATAL_ERROR "ue4ss/RE-UE4SS missing: copy ~/VSProjs/AbioticMediaPlayer/native/RE-UE4SS here (pinned at 272ce2f8, Epic deps fetched).")
endif()

add_subdirectory(RE-UE4SS)
add_subdirectory(Minefactor)
