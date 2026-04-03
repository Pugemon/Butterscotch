# =============================================================================
# Example 02: Textured Quad
# =============================================================================

add_executable(02_textured_quad main.c)
target_link_libraries(02_textured_quad PRIVATE gl2citro3d)
add_3dsx_target(02_textured_quad "gl2citro3d Textured Quad" "Textured quad example" "gl2citro3d")
