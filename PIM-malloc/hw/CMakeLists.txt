cmake_minimum_required(VERSION 3.16)

project(uMalloc)

# UPMEM SDK path - must be set by the user
if(NOT DEFINED UPMEM_HOME)
    if(DEFINED ENV{UPMEM_HOME})
        set(UPMEM_HOME $ENV{UPMEM_HOME})
    else()
        message(FATAL_ERROR
            "UPMEM_HOME is not set. Please specify the path to your UPMEM SDK installation.\n"
            "You can set it in one of the following ways:\n"
            "  1. Environment variable: export UPMEM_HOME=/path/to/upmem-sdk\n"
            "  2. CMake variable:       cmake -DUPMEM_HOME=/path/to/upmem-sdk ...\n"
            "  3. build.py argument:    python3 build.py --upmem-home /path/to/upmem-sdk\n"
        )
    endif()
endif()

set(DPU_CLANG "${UPMEM_HOME}/bin/dpu-upmem-dpurte-clang")
if(NOT EXISTS "${DPU_CLANG}")
    message(FATAL_ERROR
        "DPU compiler not found at: ${DPU_CLANG}\n"
        "Please verify that UPMEM_HOME (${UPMEM_HOME}) points to a valid UPMEM SDK installation."
    )
endif()

message(STATUS "Using UPMEM SDK at: ${UPMEM_HOME}")

add_subdirectory(Tests)
