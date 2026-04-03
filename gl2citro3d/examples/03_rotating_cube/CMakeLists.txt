# =============================================================================
# Example 03: Rotating Cube
# =============================================================================

add_executable(03_rotating_cube main.c)
target_link_libraries(03_rotating_cube PRIVATE gl2citro3d)
add_3dsx_target(03_rotating_cube "gl2citro3d Rotating Cube" "Rotating textured cube example" "gl2citro3d")
