cmake_minimum_required(VERSION 3.22.1)
project(jccshell)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fvisibility=hidden -O2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=hidden -fno-exceptions -fno-rtti -O2")

include_directories(xdl/include)

add_library(JCC SHARED
        inject_entry.cpp
        cardpool.cpp
        il2cpp_dump.cpp
        xdl/xdl.c
        xdl/xdl_iterate.c
        xdl/xdl_linker.c
        xdl/xdl_lzma.c
        xdl/xdl_util.c
)

target_link_libraries(JCC log)
