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

# Co-simulation с Spike: MMIO-доступы RISC-V-ядра к диапазону DRAM идут
# через TLM-контроллер. Требует собранный riscv-isa-sim (libriscv.so).
set(SPIKE_ROOT "/home/viktor/mipt/masters_diploma/riscv-isa-sim"
    CACHE PATH "riscv-isa-sim source/build dir")
option(DRAM_SPIKE "Build the Spike co-simulation target" ON)

if(DRAM_SPIKE AND EXISTS "${SPIKE_ROOT}/build/libriscv.so")
    add_dram_target(spike_dram
        src/dram_controller_base.cpp
        src/dram_controller_async.cpp
        src/dram_controller_sync.cpp
        src/spike_dram_device.cpp
        src/store_buffer.cpp
        src/spike_dram_main.cpp
    )
    # NB: не добавлять ${SPIKE_ROOT}/fesvr в include-пути: его syscall.h
    # затеняет системный <syscall.h> и ломает <memory>. Внутри htif.h
    # соседние заголовки подключаются относительными путями.
    target_include_directories(spike_dram PRIVATE
        "${SPIKE_ROOT}"
        "${SPIKE_ROOT}/riscv"
        "${SPIKE_ROOT}/build"
        "${SPIKE_ROOT}/softfloat"
        "${SPIKE_ROOT}/fdt"
    )
    target_link_libraries(spike_dram PRIVATE
        "${SPIKE_ROOT}/build/libriscv.so"
        "${SPIKE_ROOT}/build/libfesvr.a"
        "${SPIKE_ROOT}/build/libsoftfloat.so"
        "${SPIKE_ROOT}/build/libfdt.a"
        dl pthread z
    )
    set_property(TARGET spike_dram APPEND PROPERTY
        BUILD_RPATH "${SPIKE_ROOT}/build")
endif()

