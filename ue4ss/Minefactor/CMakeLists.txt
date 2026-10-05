add_library(Minefactor SHARED
    src/dllmain.cpp
    src/Log.cpp
    src/Link.cpp
    src/Player.cpp
    src/Input.cpp
)
set_target_properties(Minefactor PROPERTIES OUTPUT_NAME "main" PREFIX "")
target_include_directories(Minefactor PRIVATE src "${CMAKE_CURRENT_SOURCE_DIR}/../../protocol")
target_link_libraries(Minefactor PUBLIC UE4SS)
target_compile_definitions(Minefactor PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
