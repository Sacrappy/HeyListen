cmake_minimum_required(VERSION 3.20)

project(heylisten_descriptor_bench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Vulkan REQUIRED)

add_executable(descriptor_bench
    main.cpp
    shaders_spv.h
)

target_link_libraries(descriptor_bench PRIVATE Vulkan::Vulkan)

if(MSVC)
    target_compile_options(descriptor_bench PRIVATE /W4 /permissive-)
else()
    target_compile_options(descriptor_bench PRIVATE -Wall -Wextra)
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME descriptor_bench
        COMMAND descriptor_bench --frames 2 --draws 300 --warmup 1
    )
endif()
