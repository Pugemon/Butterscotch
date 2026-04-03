# =============================================================================
# Example 01: Colored Triangle
# =============================================================================

add_executable(01_triangle main.c)
target_link_libraries(01_triangle PRIVATE gl2citro3d)
add_3dsx_target(01_triangle "gl2citro3d Triangle" "Colored triangle example" "gl2citro3d")
