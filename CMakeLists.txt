cmake_minimum_required(VERSION 3.22)
project(voip_volume_fix_v3 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(voip_volume_fix SHARED voip_volume_fix.c)
add_library(samsungVoipResamplerProxy SHARED voip_resampler_proxy.c)

if(ANDROID_ABI STREQUAL "armeabi-v7a")
    target_compile_options(voip_volume_fix PRIVATE -mfpu=neon)
    target_compile_options(samsungVoipResamplerProxy PRIVATE -mfpu=neon)
endif()

target_compile_options(voip_volume_fix PRIVATE
    -Wall
    -Wextra
    -Werror
    -fvisibility=hidden
)

target_compile_options(samsungVoipResamplerProxy PRIVATE
    -Wall
    -Wextra
    -Werror
    -fvisibility=hidden
)

target_link_libraries(voip_volume_fix
    dl
    log
    m
    c
)

target_link_libraries(samsungVoipResamplerProxy
    dl
    m
    c
)

set_target_properties(voip_volume_fix PROPERTIES
    OUTPUT_NAME "libvoip_volume_fix"
    PREFIX ""
)

set_target_properties(samsungVoipResamplerProxy PROPERTIES
    OUTPUT_NAME "libsamsungVoipResampler"
    PREFIX ""
)
