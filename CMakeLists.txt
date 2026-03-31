cmake_minimum_required(VERSION 3.21)

# ------------------------------------------------------------------------------
# 1. Настройка Toolchain (Должно быть ДО project!)
# ------------------------------------------------------------------------------
set(PLATFORM "" CACHE STRING "Platform backend (glfw, sdl1, sdl2, 3ds, wiiu, ps2)")

if (PLATFORM STREQUAL "")
    message(FATAL_ERROR "PLATFORM is not set! Use -DPLATFORM=glfw (or another supported platform).")
endif()

if (PLATFORM STREQUAL "3ds" OR PLATFORM STREQUAL "wiiu")
    if (NOT DEFINED CMAKE_TOOLCHAIN_FILE)
        if (NOT DEFINED ENV{DEVKITPRO})
            message(FATAL_ERROR "Please define DEVKITPRO to point to your SDK path!")
        endif ()

        if (PLATFORM STREQUAL "3ds")
            set(CMAKE_TOOLCHAIN_FILE "$ENV{DEVKITPRO}/cmake/3DS.cmake" CACHE PATH "toolchain file")
        elseif (PLATFORM STREQUAL "wiiu")
            set(CMAKE_TOOLCHAIN_FILE "$ENV{DEVKITPRO}/cmake/WiiU.cmake" CACHE PATH "toolchain file")
        endif()
    endif ()
endif ()

project(butterscotch C)

# ------------------------------------------------------------------------------
# 2. Глобальные опции проекта
# ------------------------------------------------------------------------------
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(ENABLE_ASAN "Enable AddressSanitizer (Memory debug)" OFF)
option(ENABLE_VM_TRACING "Enable VM opcode and variable tracing logs" OFF)

# Метаданные
set(BUTTERSCOTCH_COMMIT_HASH "unknown" CACHE STRING "Git commit hash")
set(BUTTERSCOTCH_COMMIT_DATE "unknown" CACHE STRING "Git commit date")

# ------------------------------------------------------------------------------
# 3. Базовый исполняемый файл
# ------------------------------------------------------------------------------
file(GLOB CORE_SOURCES src/*.c)
add_executable(butterscotch ${CORE_SOURCES})

# Базовые флаги и инклюды для ВСЕХ платформ
target_compile_options(butterscotch PRIVATE -Wall -Wextra)
target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_definitions(butterscotch PRIVATE
        BUTTERSCOTCH_COMMIT_HASH="${BUTTERSCOTCH_COMMIT_HASH}"
        BUTTERSCOTCH_COMMIT_DATE="${BUTTERSCOTCH_COMMIT_DATE}"
)

# Общие сторонние библиотеки (stb, miniaudio)
target_include_directories(butterscotch PUBLIC
        vendor/stb/ds
        vendor/stb/image
        vendor/stb/vorbis
        vendor/miniaudio
)

# Управление дефайном трассировки из CMake
if (ENABLE_VM_TRACING)
    target_compile_definitions(butterscotch PRIVATE VM_ENABLE_TRACING)
endif()

# Управление ASAN
if (ENABLE_ASAN)
    target_compile_options(butterscotch PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(butterscotch PRIVATE -fsanitize=address)
endif()

# ------------------------------------------------------------------------------
# 4. Специфичные настройки платформ (Platform Backends)
# ------------------------------------------------------------------------------

if (PLATFORM STREQUAL "glfw")
    file(GLOB PLATFORM_SOURCES src/glfw/*.c)
    target_sources(butterscotch PRIVATE ${PLATFORM_SOURCES})

    # GLAD
    add_library(glad STATIC vendor/glad/src/glad.c)
    target_include_directories(glad PUBLIC vendor/glad/include)

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLFW3 REQUIRED glfw3)

    target_include_directories(butterscotch PRIVATE ${GLFW3_INCLUDE_DIRS})
    target_link_libraries(butterscotch PRIVATE ${GLFW3_LIBRARIES} glad m pthread dl)

elseif (PLATFORM STREQUAL "sdl1")
    file(GLOB PLATFORM_SOURCES src/sdl12/*.c)
    target_sources(butterscotch PRIVATE ${PLATFORM_SOURCES})
    target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/sdl12)

    target_compile_definitions(butterscotch PRIVATE
            USE_FLOAT_REALS
            BUTTERSCOTCH_PLATFORM_SDL1
    )

    if (CMAKE_COMPILER_IS_GNUCC OR CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_link_options(butterscotch PRIVATE -static-libgcc)
    endif()

    find_package(SDL REQUIRED)
    target_include_directories(butterscotch PRIVATE ${SDL_INCLUDE_DIR})
    target_link_libraries(butterscotch PRIVATE ${SDL_LIBRARY} SDL_mixer m pthread)

    if (WIN32)
        target_link_libraries(butterscotch PRIVATE
                mingw32 user32 gdi32 winmm imm32 ole32 oleaut32
                version uuid advapi32 setupapi shell32 dinput8 dxguid
        )
    endif()

elseif (PLATFORM STREQUAL "sdl2")
    file(GLOB PLATFORM_SOURCES src/sdl2/*.c)
    target_sources(butterscotch PRIVATE ${PLATFORM_SOURCES})

    target_compile_definitions(butterscotch PRIVATE BUTTERSCOTCH_PLATFORM_SDL2)

    if (CMAKE_COMPILER_IS_GNUCC OR CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_link_options(butterscotch PRIVATE -static -static-libgcc)
    endif()

    find_package(SDL2 REQUIRED)

    if (TARGET SDL2::SDL2-static)
        target_link_libraries(butterscotch PRIVATE SDL2::SDL2-static)
    else ()
        target_link_libraries(butterscotch PRIVATE SDL2::SDL2main SDL2::SDL2)
    endif()

    target_link_libraries(butterscotch PRIVATE m pthread)

    if (WIN32)
        target_link_libraries(butterscotch PRIVATE
                mingw32 user32 gdi32 winmm imm32 ole32 oleaut32
                version uuid advapi32 setupapi shell32 dinput8 SDL2_mixer
        )
    endif()

elseif (PLATFORM STREQUAL "3ds")
    file(GLOB PLATFORM_SOURCES src/3ds/*.c src/3ds/c3d/*.c src/sdl12/*.c)
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/sdl12/main.c")
    list(REMOVE_ITEM PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/sdl12/sdl12_renderer.c")

    target_sources(butterscotch PRIVATE ${PLATFORM_SOURCES})

    target_include_directories(butterscotch PRIVATE
            ${CMAKE_SOURCE_DIR}/src/sdl12
            ${CMAKE_SOURCE_DIR}/src/3ds
            ${CMAKE_SOURCE_DIR}/src/3ds/c3d
    )

    # Shader pipeline
    ctr_add_shader_library(butterscotch_shader ${CMAKE_SOURCE_DIR}/src/3ds/shaders/shader.v.pica)
    dkp_add_embedded_binary_library(shaders butterscotch_shader)

    target_include_directories(butterscotch PRIVATE "$ENV{DEVKITPRO}/portlibs/3ds/include")
    target_link_libraries(butterscotch PRIVATE
            shaders
            SDL_mixer mad vorbisidec ogg mikmod
            SDL SDL_image m pthread citro2d citro3d ctru
    )

    target_compile_definitions(butterscotch PRIVATE
            __3DS__
            USE_FLOAT_REALS
            BUTTERSCOTCH_PLATFORM_SDL1
    )

    target_compile_options(butterscotch PRIVATE
            $<$<CONFIG:Debug>:-g3 -O0>        # Debug: полная отладка, без оптимизации
            $<$<CONFIG:Release>:-O3 -DNDEBUG> # Release: максимальная оптимизация
    )

    # 3DSX
    ctr_generate_smdh(${PROJECT_NAME}.smdh
            NAME "Butterscotch"
            DESCRIPTION "GMS 16 Runner for 3DS"
            AUTHOR "PG Team"
    )

    ctr_create_3dsx(${PROJECT_NAME} SMDH ${PROJECT_NAME}.smdh)

elseif (PLATFORM STREQUAL "wiiu")
    file(GLOB PLATFORM_SOURCES src/wiiu/*.c)
    target_sources(butterscotch PRIVATE ${PLATFORM_SOURCES})

    target_compile_definitions(butterscotch PRIVATE
            __CAFE__
            __WIIU__
            NDEBUG
            USE_FLOAT_REALS
            BUTTERSCOTCH_PLATFORM_WIIU
    )

    target_compile_options(butterscotch PRIVATE
            -O3 -mcpu=750 -meabi -mhard-float -ffast-math -funroll-loops
    )

    target_include_directories(butterscotch PRIVATE
            $ENV{DEVKITPRO}/wut/include
            $ENV{DEVKITPRO}/portlibs/wiiu/include
    )
    target_link_directories(butterscotch PRIVATE
            $ENV{DEVKITPRO}/wut/lib
            $ENV{DEVKITPRO}/portlibs/wiiu/lib
    )

    target_link_libraries(butterscotch PRIVATE SDL2 SDL2_mixer SDL2_image wut m)
    set_target_properties(butterscotch PROPERTIES SUFFIX ".rpx")

elseif (PLATFORM STREQUAL "ps2")
    file(GLOB PLATFORM_SOURCES src/ps2/*.c)
    target_sources(butterscotch PRIVATE ${PLATFORM_SOURCES})

    target_compile_definitions(butterscotch PRIVATE USE_FLOAT_REALS)
    target_compile_options(butterscotch PRIVATE -fsingle-precision-constant)

    if (PS2) # Сборка внутри контейнера
        target_include_directories(butterscotch PRIVATE
                $ENV{PS2SDK}/ee/include $ENV{PS2SDK}/common/include
                $ENV{PS2DEV}/ee/include $ENV{GSKIT}/include
        )
        target_link_directories(butterscotch PRIVATE $ENV{PS2SDK}/ee/lib $ENV{GSKIT}/lib)
        target_link_libraries(butterscotch PRIVATE gskit dmakit audsrv pad mc patches kernel c m)

        set(IRX_DIR $ENV{PS2SDK}/iop/irx)
        set(IRX_GEN_DIR ${CMAKE_BINARY_DIR}/generated/irx)
        file(MAKE_DIRECTORY ${IRX_GEN_DIR})

        macro(embed_irx IRX_NAME)
            add_custom_command(
                    OUTPUT ${IRX_GEN_DIR}/${IRX_NAME}_irx.c
                    COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/${IRX_NAME}.irx ${IRX_GEN_DIR}/${IRX_NAME}_irx.c ${IRX_NAME}_irx
                    DEPENDS ${IRX_DIR}/${IRX_NAME}.irx
                    COMMENT "Embedding ${IRX_NAME}.irx"
            )
            target_sources(butterscotch PRIVATE ${IRX_GEN_DIR}/${IRX_NAME}_irx.c)
        endmacro()

        embed_irx(sio2man)
        embed_irx(mcman)
        embed_irx(mcserv)
        embed_irx(padman)
        embed_irx(freesd)
        embed_irx(audsrv)

        target_include_directories(butterscotch PRIVATE ${IRX_GEN_DIR})
    else () # CLion IDE
        file(GLOB PS2SDK_EE_INCLUDES ${CMAKE_SOURCE_DIR}/ps2sdk/ee/*/include)
        file(GLOB PS2SDK_EE_RPC_INCLUDES ${CMAKE_SOURCE_DIR}/ps2sdk/ee/rpc/*/include)
        file(GLOB PS2SDK_EE_NETWORK_INCLUDES ${CMAKE_SOURCE_DIR}/ps2sdk/ee/network/*/include)
        file(GLOB GSKIT_INCLUDES ${CMAKE_SOURCE_DIR}/gsKit/ee/*/include)

        target_compile_definitions(butterscotch PRIVATE _EE)
        target_include_directories(butterscotch PRIVATE
                ${PS2SDK_EE_INCLUDES}
                ${PS2SDK_EE_RPC_INCLUDES}
                ${PS2SDK_EE_NETWORK_INCLUDES}
                ${CMAKE_SOURCE_DIR}/ps2sdk/common/include
                ${GSKIT_INCLUDES}
                ${CMAKE_SOURCE_DIR}/ps2dev-headers/newlib-include
                ${CMAKE_SOURCE_DIR}/ps2dev-headers/gcc-include
        )
    endif()
endif()