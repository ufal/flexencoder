cmake_minimum_required(VERSION 3.16)
project(flexicorp_pando VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Ensure vendored/static deps (notably pando_core) are PIC so they can be linked
# into libflexicorp_pando on aarch64/arm64.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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

# The CLI links against libflexicorp_pando. Without explicit rpath, macOS/Linux
# often embed the *build tree* path; copying only flexicorp-pando to e.g.
# /usr/local/bin then fails with "Library not loaded: @rpath/libflexicorp_pando".
# @loader_path / $ORIGIN keeps the dylib next to the binary in the build dir;
# cmake --install places lib/ and bin/ so ../lib resolves.
if(APPLE)
	set_target_properties(flexicorp-pando PROPERTIES
		MACOSX_RPATH ON
		BUILD_RPATH "@loader_path"
		INSTALL_RPATH "@loader_path/../lib"
	)
else()
	set_target_properties(flexicorp-pando PROPERTIES
		BUILD_RPATH "\$ORIGIN"
		INSTALL_RPATH "\$ORIGIN/../lib"
	)
endif()

# Optional: prefix install (e.g. cmake --install . --prefix /usr/local)
install(TARGETS flexicorp_pando flexicorp-pando
	LIBRARY DESTINATION lib
	RUNTIME DESTINATION bin
)
