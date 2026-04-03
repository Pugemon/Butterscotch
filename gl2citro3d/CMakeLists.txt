# =============================================================================
# gl2citro3d - OpenGL ES 1.1 -> Citro3D Translation Layer
# =============================================================================
# Build:
#   mkdir build && cd build
#   cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/DevkitArm3DS.cmake ..
#   make
# =============================================================================
cmake_minimum_required(VERSION 3.13)
project(gl2citro3d C ASM)

# Build library
add_subdirectory(src)

# Build examples
option(GL2CITRO3D_BUILD_EXAMPLES "Build examples" ON)
if(GL2CITRO3D_BUILD_EXAMPLES)
    add_subdirectory(examples/01_triangle)
    add_subdirectory(examples/02_textured_quad)
    add_subdirectory(examples/03_rotating_cube)
endif()
