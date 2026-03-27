cmake_minimum_required(VERSION 3.21)
if(PLATFORM STREQUAL "3ds")
    if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
        if(DEFINED ENV{DEVKITPRO})
            set(CMAKE_TOOLCHAIN_FILE "$ENV{DEVKITPRO}/cmake/3DS.cmake" CACHE PATH "toolchain file")
        else()
            message(FATAL_ERROR "Please define DEVKITPRO to point to your SDK path!")
        endif()
    endif()
    add_definitions(-D__3DS__)
endif()
if(PLATFORM STREQUAL "wiiu")
    if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
        if(DEFINED ENV{DEVKITPRO})
            set(CMAKE_TOOLCHAIN_FILE "$ENV{DEVKITPRO}/cmake/WiiU.cmake" CACHE PATH "toolchain file")
        else()
            message(FATAL_ERROR "Please define DEVKITPRO to point to your SDK path!")
        endif()
    endif()
    add_definitions(-D__CAFE__)
endif()
project(butterscotch C)

if (PLATFORM STREQUAL "3ds")
    set(CTR_APP_NAME "Butterscotch")
    set(CTR_APP_AUTHOR "PG Team")
    set(CTR_APP_DESC "GMS 16 Runner for 3DS")
endif()
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

set(PLATFORM "" CACHE STRING "Platform backend")

# Build metadata
set(BUTTERSCOTCH_COMMIT_HASH "unknown" CACHE STRING "Git commit hash")
set(BUTTERSCOTCH_COMMIT_DATE "unknown" CACHE STRING "Git commit date")
add_compile_definitions(
    BUTTERSCOTCH_COMMIT_HASH="${BUTTERSCOTCH_COMMIT_HASH}"
    BUTTERSCOTCH_COMMIT_DATE="${BUTTERSCOTCH_COMMIT_DATE}"
)

if(PLATFORM STREQUAL "")
    message(FATAL_ERROR "PLATFORM is not set! Use -DPLATFORM=glfw (or another supported platform).")
endif()

file(GLOB SOURCES src/*.c)
# Platform specific files builds
file(GLOB PLATFORM_SOURCES src/${PLATFORM}/*.c)

add_executable(butterscotch ${SOURCES} ${PLATFORM_SOURCES})
target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src)

# stb_ds
target_include_directories(butterscotch PUBLIC vendor/stb/ds)

if(PLATFORM STREQUAL "glfw")
    # GLAD
    add_library(glad STATIC vendor/glad/src/glad.c)
    target_include_directories(glad PUBLIC vendor/glad/include)

    # stb_image
    target_include_directories(butterscotch PUBLIC vendor/stb/image)

    # stb_vorbis
    target_include_directories(butterscotch PUBLIC vendor/stb/vorbis)

    # miniaudio
    target_include_directories(butterscotch PUBLIC vendor/miniaudio)

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLFW3 REQUIRED glfw3)
    target_include_directories(butterscotch PRIVATE ${GLFW3_INCLUDE_DIRS})
    target_link_libraries(butterscotch ${GLFW3_LIBRARIES} glad m pthread dl)

    # Enable useful things to figure out when things have gone wrong related to memory corruption
    # (Also set -DCMAKE_BUILD_TYPE=Debug to get debug symbols!)
    set(ENABLE_ASAN OFF CACHE BOOL "Enable AddressSanitizer")
    if(ENABLE_ASAN)
        target_compile_options(butterscotch PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(butterscotch PRIVATE -fsanitize=address)
    endif()
elseif(PLATFORM STREQUAL "ps2")
    # The PS2 EE has a single-precision FPU only, so double is software-emulated
    add_compile_definitions(USE_FLOAT_REALS)
    # Treat unsuffixed float constants as single-precision to avoid accidental
    # float->double promotions on platforms with no hardware double
    add_compile_options(-fsingle-precision-constant)

    if(PS2)
        # Compiling on the Docker container
        target_include_directories(butterscotch PRIVATE $ENV{PS2SDK}/ee/include $ENV{PS2SDK}/common/include $ENV{PS2DEV}/ee/include $ENV{GSKIT}/include)
        target_link_directories(butterscotch PRIVATE $ENV{PS2SDK}/ee/lib $ENV{GSKIT}/lib)
        target_link_libraries(butterscotch gskit dmakit audsrv pad mc patches kernel c m)

        # Embed ps2sdk IRX modules (sio2man, padman) as C arrays via bin2c
        # This avoids loading from rom0: which has compatibility issues between fat/slim PS2
        set(IRX_DIR $ENV{PS2SDK}/iop/irx)
        set(IRX_GEN_DIR ${CMAKE_BINARY_DIR}/generated/irx)
        file(MAKE_DIRECTORY ${IRX_GEN_DIR})

        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/sio2man_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/sio2man.irx ${IRX_GEN_DIR}/sio2man_irx.c sio2man_irx
            DEPENDS ${IRX_DIR}/sio2man.irx
            COMMENT "Embedding sio2man.irx"
        )
        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/mcman_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/mcman.irx ${IRX_GEN_DIR}/mcman_irx.c mcman_irx
            DEPENDS ${IRX_DIR}/mcman.irx
            COMMENT "Embedding mcman.irx"
        )
        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/mcserv_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/mcserv.irx ${IRX_GEN_DIR}/mcserv_irx.c mcserv_irx
            DEPENDS ${IRX_DIR}/mcserv.irx
            COMMENT "Embedding mcserv.irx"
        )
        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/padman_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/padman.irx ${IRX_GEN_DIR}/padman_irx.c padman_irx
            DEPENDS ${IRX_DIR}/padman.irx
            COMMENT "Embedding padman.irx"
        )
        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/freesd_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/freesd.irx ${IRX_GEN_DIR}/freesd_irx.c freesd_irx
            DEPENDS ${IRX_DIR}/freesd.irx
            COMMENT "Embedding freesd.irx"
        )
        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/audsrv_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/audsrv.irx ${IRX_GEN_DIR}/audsrv_irx.c audsrv_irx
            DEPENDS ${IRX_DIR}/audsrv.irx
            COMMENT "Embedding audsrv.irx"
        )

        target_sources(butterscotch PRIVATE ${IRX_GEN_DIR}/sio2man_irx.c ${IRX_GEN_DIR}/mcman_irx.c ${IRX_GEN_DIR}/mcserv_irx.c ${IRX_GEN_DIR}/padman_irx.c ${IRX_GEN_DIR}/freesd_irx.c ${IRX_GEN_DIR}/audsrv_irx.c)
        target_include_directories(butterscotch PRIVATE ${IRX_GEN_DIR})
    else()
        # This block is for when we are developing Butterscotch with CLion!
        # ps2sdk and gsKit scatter headers across per-module subdirectories;
        # the flat include/ layout only exists after building the SDK.
        # We use file(GLOB) to discover all include dirs automatically.
        file(GLOB PS2SDK_EE_INCLUDES ${CMAKE_SOURCE_DIR}/ps2sdk/ee/*/include)
        file(GLOB PS2SDK_EE_RPC_INCLUDES ${CMAKE_SOURCE_DIR}/ps2sdk/ee/rpc/*/include)
        file(GLOB PS2SDK_EE_NETWORK_INCLUDES ${CMAKE_SOURCE_DIR}/ps2sdk/ee/network/*/include)
        file(GLOB GSKIT_INCLUDES ${CMAKE_SOURCE_DIR}/gsKit/ee/*/include)

        # _EE is normally set by the PS2 toolchain; without it, tamtypes.h
        # refuses to define u64/u32/etc. We set it so CLion can resolve them.
        target_compile_definitions(butterscotch PRIVATE _EE)

        target_include_directories(butterscotch PRIVATE
                ${PS2SDK_EE_INCLUDES}
                ${PS2SDK_EE_RPC_INCLUDES}
                ${PS2SDK_EE_NETWORK_INCLUDES}
                ${CMAKE_SOURCE_DIR}/ps2sdk/common/include
                ${GSKIT_INCLUDES}
                # Newlib and GCC built-in headers are build artifacts (not in any repo),
                # so these are still extracted from Docker by setup.sh.
                ${CMAKE_SOURCE_DIR}/ps2dev-headers/newlib-include
                ${CMAKE_SOURCE_DIR}/ps2dev-headers/gcc-include
        )
    endif()
elseif(PLATFORM STREQUAL "sdl2")
    # stb_image, stb_vorbis, miniaudio
    add_compile_definitions(BUTTERSCOTCH_PLATFORM_SDL2)

    target_include_directories(butterscotch PUBLIC vendor/stb/image)
    target_include_directories(butterscotch PUBLIC vendor/stb/vorbis)
    target_include_directories(butterscotch PUBLIC vendor/miniaudio)

    if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_link_options(butterscotch PRIVATE -static -static-libgcc)
    endif()

    find_package(SDL2 REQUIRED)

    if(TARGET SDL2::SDL2-static)
        target_link_libraries(butterscotch PRIVATE SDL2::SDL2-static)
    else()
        target_link_libraries(butterscotch PRIVATE SDL2::SDL2main SDL2::SDL2)
    endif()

    if(WIN32)
        target_link_libraries(butterscotch PRIVATE
                mingw32 user32 gdi32 winmm imm32 ole32 oleaut32
                version uuid advapi32 setupapi shell32 dinput8
                SDL2_mixer
        )
    endif()

    target_link_libraries(butterscotch PRIVATE m pthread)

    # Enable AddressSanitizer optionally
    set(ENABLE_ASAN OFF CACHE BOOL "Enable AddressSanitizer")
    if(ENABLE_ASAN)
        target_compile_options(butterscotch PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(butterscotch PRIVATE -fsanitize=address)
    endif()
elseif(PLATFORM STREQUAL "3ds")
    # stb_image, stb_vorbis, miniaudio
    add_compile_definitions(BUTTERSCOTCH_PLATFORM_SDL2)

    target_include_directories(butterscotch PUBLIC vendor/stb/image)
    target_include_directories(butterscotch PUBLIC vendor/stb/vorbis)
    target_include_directories(butterscotch PUBLIC vendor/miniaudio)

    target_compile_options(butterscotch PRIVATE -O3)

    target_compile_options(butterscotch PRIVATE
            -march=armv6k
            -mtune=mpcore
            -mfpu=vfpv2
            -mfloat-abi=hard
            -marm
            -fomit-frame-pointer
    )

    target_compile_options(butterscotch PRIVATE
            -ffast-math
            -fsingle-precision-constant
            -funroll-loops
    )

    target_compile_options(butterscotch PRIVATE
            -fno-stack-protector
            -fno-math-errno
            -fassociative-math
            -ffinite-math-only
            -flto
    )

    target_compile_definitions(butterscotch PRIVATE
            NDEBUG
            _3DS
            USE_FLOAT_REALS
    )

    target_link_options(butterscotch PRIVATE -O3)

    target_include_directories(butterscotch PRIVATE "$ENV{DEVKITPRO}/portlibs/3ds/include")
    target_link_libraries(butterscotch PRIVATE SDL_mixer mad
            vorbisidec
            ogg
            mikmod
            SDL SDL_image picaGL m pthread citro2d citro3d ctru)

    ctr_generate_smdh(${PROJECT_NAME}.smdh
            NAME ${CTR_APP_NAME}
            DESCRIPTION ${CTR_APP_DESC}
            AUTHOR ${CTR_APP_AUTHOR}
    )

    ctr_create_3dsx(${PROJECT_NAME}
            SMDH ${PROJECT_NAME}.smdh
            # ROMFS ${ROMFS_STAGING_DIR}
    )
elseif(PLATFORM STREQUAL "wiiu")
    # Определение платформы
    add_compile_definitions(BUTTERSCOTCH_PLATFORM_WIIU)

    # stb / audio / image
    target_include_directories(butterscotch PUBLIC vendor/stb/image)
    target_include_directories(butterscotch PUBLIC vendor/stb/vorbis)
    target_include_directories(butterscotch PUBLIC vendor/miniaudio)

    # Оптимизация (Wii U = PowerPC)
    target_compile_options(butterscotch PRIVATE
            -O3
            -mcpu=750
            -meabi
            -mhard-float
            -ffast-math
            -funroll-loops
    )

    target_compile_definitions(butterscotch PRIVATE
            NDEBUG
            __WIIU__
            USE_FLOAT_REALS
    )

    # devkitPro пути
    target_include_directories(butterscotch PRIVATE
            $ENV{DEVKITPRO}/wut/include
            $ENV{DEVKITPRO}/portlibs/wiiu/include
    )

    target_link_directories(butterscotch PRIVATE
            $ENV{DEVKITPRO}/wut/lib
            $ENV{DEVKITPRO}/portlibs/wiiu/lib
    )

    # Линковка (WUT + стандартные либы)
    target_link_libraries(butterscotch PRIVATE
            SDL2
            SDL2_mixer
            SDL2_image

            wut
            m
    )

    # Генерация .rpx
    set_target_properties(butterscotch PROPERTIES SUFFIX ".rpx")
else()
    message(FATAL_ERROR "Unknown platform! ${PLATFORM}")
endif()
