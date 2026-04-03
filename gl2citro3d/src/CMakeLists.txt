# =============================================================================
# gl2citro3d static library
# =============================================================================

set(SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/gl2citro3d_shader.pica")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")

# Compile .pica -> .shbin -> embedded .s + .h
pica_compile_shader("${SHADER_SRC}" "${GENERATED_DIR}" SHADER_S SHADER_H)

# Library sources
add_library(gl2citro3d STATIC
    gl2citro3d.c
    ${SHADER_S}
)

target_include_directories(gl2citro3d
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    PRIVATE
        "${GENERATED_DIR}"
)

target_link_libraries(gl2citro3d
    PUBLIC
        citro3d
        ctru
        m
)
