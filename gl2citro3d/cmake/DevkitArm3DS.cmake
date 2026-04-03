# =============================================================================
# DevKitARM 3DS CMake Toolchain File
# =============================================================================
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/DevkitArm3DS.cmake ..
# =============================================================================

if(NOT DEFINED ENV{DEVKITPRO})
    message(FATAL_ERROR "DEVKITPRO environment variable not set. "
            "Install devkitPro: https://devkitpro.org/wiki/Getting_Started")
endif()

if(NOT DEFINED ENV{DEVKITARM})
    message(FATAL_ERROR "DEVKITARM environment variable not set.")
endif()

set(DEVKITPRO $ENV{DEVKITPRO})
set(DEVKITARM $ENV{DEVKITARM})

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR armv6k)
set(CMAKE_CROSSCOMPILING TRUE)

# Compilers
set(CMAKE_C_COMPILER   "${DEVKITARM}/bin/arm-none-eabi-gcc")
set(CMAKE_CXX_COMPILER "${DEVKITARM}/bin/arm-none-eabi-g++")
set(CMAKE_ASM_COMPILER "${DEVKITARM}/bin/arm-none-eabi-gcc")
set(CMAKE_AR           "${DEVKITARM}/bin/arm-none-eabi-ar" CACHE FILEPATH "Archiver")
set(CMAKE_RANLIB       "${DEVKITARM}/bin/arm-none-eabi-ranlib" CACHE FILEPATH "Ranlib")
set(CMAKE_STRIP        "${DEVKITARM}/bin/arm-none-eabi-strip" CACHE FILEPATH "Strip")

# 3DS arch flags
set(ARCH_FLAGS "-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft")

set(CMAKE_C_FLAGS_INIT   "${ARCH_FLAGS} -mword-relocations -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_INIT "${ARCH_FLAGS} -mword-relocations -ffunction-sections -fdata-sections -fno-rtti -fno-exceptions")
set(CMAKE_ASM_FLAGS_INIT "${ARCH_FLAGS}")

# Search paths
set(CMAKE_FIND_ROOT_PATH
    "${DEVKITPRO}/libctru"
    "${DEVKITPRO}/portlibs/3ds"
    "${DEVKITARM}/arm-none-eabi"
)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Standard include/lib dirs
include_directories(SYSTEM
    "${DEVKITPRO}/libctru/include"
    "${DEVKITPRO}/portlibs/3ds/include"
)

link_directories(
    "${DEVKITPRO}/libctru/lib"
    "${DEVKITPRO}/portlibs/3ds/lib"
)

# Linker specs for 3dsx
set(CMAKE_EXE_LINKER_FLAGS_INIT "-specs=3dsx.specs ${ARCH_FLAGS}")

# Tools
find_program(PICASSO_EXE picasso HINTS "${DEVKITPRO}/tools/bin" "${DEVKITARM}/bin")
find_program(BIN2S_EXE   bin2s   HINTS "${DEVKITPRO}/tools/bin" "${DEVKITARM}/bin")
find_program(3DSXTOOL    3dsxtool HINTS "${DEVKITPRO}/tools/bin" "${DEVKITARM}/bin")
find_program(SMDHTOOL    smdhtool HINTS "${DEVKITPRO}/tools/bin" "${DEVKITARM}/bin")

if(NOT PICASSO_EXE)
    message(FATAL_ERROR "picasso not found. Install devkitPro 3DS tools.")
endif()
if(NOT BIN2S_EXE)
    message(FATAL_ERROR "bin2s not found. Install devkitPro 3DS tools.")
endif()

# =============================================================================
# Helper: compile .pica shader -> .shbin -> .s + .h (embedded binary)
# =============================================================================
# Usage: pica_compile_shader(SHADER_SRC OUTPUT_DIR GENERATED_S GENERATED_H)
function(pica_compile_shader SHADER_SRC OUTPUT_DIR OUT_S OUT_H)
    get_filename_component(SHADER_NAME "${SHADER_SRC}" NAME_WE)
    set(_SHBIN "${OUTPUT_DIR}/${SHADER_NAME}.shbin")
    set(_S     "${OUTPUT_DIR}/${SHADER_NAME}_shbin.s")
    set(_H     "${OUTPUT_DIR}/${SHADER_NAME}_shbin.h")

    add_custom_command(
        OUTPUT "${_SHBIN}"
        COMMAND "${PICASSO_EXE}" -o "${_SHBIN}" "${SHADER_SRC}"
        DEPENDS "${SHADER_SRC}"
        COMMENT "PICASSO ${SHADER_NAME}.pica -> ${SHADER_NAME}.shbin"
    )

    add_custom_command(
        OUTPUT "${_S}" "${_H}"
        COMMAND "${BIN2S_EXE}" "${_SHBIN}" > "${_S}"
        COMMAND ${CMAKE_COMMAND} -E echo
                "extern const unsigned char ${SHADER_NAME}_shbin[]$<SEMICOLON>"
                > "${_H}"
        COMMAND ${CMAKE_COMMAND} -E echo
                "extern const unsigned int ${SHADER_NAME}_shbin_size$<SEMICOLON>"
                >> "${_H}"
        DEPENDS "${_SHBIN}"
        COMMENT "BIN2S ${SHADER_NAME}.shbin -> embedded .s + .h"
    )

    set(${OUT_S} "${_S}" PARENT_SCOPE)
    set(${OUT_H} "${_H}" PARENT_SCOPE)
endfunction()

# =============================================================================
# Helper: create .3dsx from ELF target
# =============================================================================
# Usage: add_3dsx_target(TARGET_NAME APP_TITLE APP_DESC APP_AUTHOR)
function(add_3dsx_target TARGET_NAME APP_TITLE APP_DESC APP_AUTHOR)
    if(3DSXTOOL)
        set(_3DSX "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.3dsx")
        if(SMDHTOOL)
            set(_SMDH "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.smdh")
            add_custom_command(
                TARGET ${TARGET_NAME} POST_BUILD
                COMMAND "${SMDHTOOL}" --create "${APP_TITLE}" "${APP_DESC}" "${APP_AUTHOR}" "${_SMDH}"
                COMMAND "${3DSXTOOL}" "$<TARGET_FILE:${TARGET_NAME}>" "${_3DSX}" "--smdh=${_SMDH}"
                COMMENT "3DSXTOOL ${TARGET_NAME} -> ${TARGET_NAME}.3dsx"
            )
        else()
            add_custom_command(
                TARGET ${TARGET_NAME} POST_BUILD
                COMMAND "${3DSXTOOL}" "$<TARGET_FILE:${TARGET_NAME}>" "${_3DSX}"
                COMMENT "3DSXTOOL ${TARGET_NAME} -> ${TARGET_NAME}.3dsx"
            )
        endif()
    endif()
endfunction()
