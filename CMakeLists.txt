cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_CXX_COMPILER)
    find_program(CLANGXX_EXECUTABLE clang++)
    if(CLANGXX_EXECUTABLE)
        set(CMAKE_CXX_COMPILER "${CLANGXX_EXECUTABLE}")
        message(STATUS "Using clang++: ${CLANGXX_EXECUTABLE}")
    endif()
endif()

project(dram_models LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SYSTEMC_ROOT "/home/viktor/mipt/masters_diploma/systemc-cxx26"
    CACHE PATH "SystemC install prefix (must match CMAKE_CXX_STANDARD)")
list(APPEND CMAKE_PREFIX_PATH "${SYSTEMC_ROOT}")

find_package(SystemCLanguage CONFIG REQUIRED)
find_package(Threads REQUIRED)



function(add_dram_target target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE include)
    target_link_libraries(${target} PRIVATE SystemC::systemc Threads::Threads)
    target_compile_options(${target} PRIVATE -Wall -Wextra)


    # Позволяет запускать собранный бинарник без LD_LIBRARY_PATH.
    set_target_properties(${target} PROPERTIES
        BUILD_RPATH "$<TARGET_FILE_DIR:SystemC::systemc>")
endfunction()

# DRAM-контроллер с read/write очередями и арбитражем.
add_dram_target(dram_controller
    src/dram_controller_base.cpp
    src/dram_controller_async.cpp
    src/dram_controller_sync.cpp
    src/controller_main.cpp
)

