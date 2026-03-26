cmake_minimum_required(VERSION 3.16)
project(flexicorp_pando VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Pando dependency ──────────────────────────────────────────────────────
# Point PANDO_DIR at the Pando source tree (where CMakeLists.txt and src/ are).
# Default: assume Pando is a sibling directory ../../pando relative to this file.
if(NOT DEFINED PANDO_DIR)
    set(PANDO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../pando"
        CACHE PATH "Path to Pando source tree")
endif()

if(NOT TARGET pando_core)
    add_subdirectory("${PANDO_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/pando_build")
endif()

# ── Shared library: libflexicorp_pando.so ─────────────────────────────────
# This is the primary build target — loaded by PHP via FFI.
add_library(flexicorp_pando SHARED flexicorp_pando.cpp)
target_include_directories(flexicorp_pando
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${PANDO_DIR}/src
)
target_link_libraries(flexicorp_pando PRIVATE pando_api pando_core)

# Ensure symbols are exported for FFI.
set_target_properties(flexicorp_pando PROPERTIES
    C_VISIBILITY_PRESET   default
    CXX_VISIBILITY_PRESET default
    POSITION_INDEPENDENT_CODE ON
)

# ── CLI executable: flexicorp-pando ───────────────────────────────────────
# Drop-in C++ replacement for dev/flexicorp-pando.py.
add_executable(flexicorp-pando main.cpp)
target_include_directories(flexicorp-pando PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(flexicorp-pando PRIVATE flexicorp_pando)

# ── Daemon: flexicorp-pando-server ────────────────────────────────────────
# Unix socket server — keeps corpora open, serves multiple PHP workers.
add_executable(flexicorp-pando-server server.cpp)
target_include_directories(flexicorp-pando-server
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${PANDO_DIR}/src
)
target_link_libraries(flexicorp-pando-server PRIVATE pando_api pando_core)
find_package(Threads REQUIRED)
target_link_libraries(flexicorp-pando-server PRIVATE Threads::Threads)
