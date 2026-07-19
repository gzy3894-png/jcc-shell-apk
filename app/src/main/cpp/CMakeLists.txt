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

# 真机极简注入：只 dlopen，不装原版资源/特效 Hook
add_executable(jcc_inject mini_inject.cpp)

target_link_libraries(JCC log)
target_link_libraries(jcc_inject log dl)
