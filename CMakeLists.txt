cmake_minimum_required(VERSION 3.21)

# Select the Android target before vcpkg and the NDK initialize their toolchains.
# Explicit cache values allow configuring without a connected device.
if(NOT ANDROID_ABI OR NOT ANDROID_PLATFORM)
    find_program(ADB_EXECUTABLE adb REQUIRED)
endif()

if(NOT ANDROID_ABI)
    execute_process(
        COMMAND "${ADB_EXECUTABLE}" shell getprop ro.product.cpu.abi
        OUTPUT_VARIABLE device_abi
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
    )
    set(ANDROID_ABI "${device_abi}" CACHE STRING "Android target ABI")
endif()

if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(android_triplet arm64-android)
elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
    set(android_triplet arm-android)
elseif(ANDROID_ABI STREQUAL "x86_64")
    set(android_triplet x64-android)
elseif(ANDROID_ABI STREQUAL "x86")
    set(android_triplet x86-android)
else()
    message(FATAL_ERROR "Unsupported Android ABI: ${ANDROID_ABI}")
endif()
set(VCPKG_TARGET_TRIPLET "${android_triplet}" CACHE STRING "vcpkg Android triplet")

if(NOT ANDROID_PLATFORM)
    execute_process(
        COMMAND "${ADB_EXECUTABLE}" shell getprop ro.build.version.sdk
        OUTPUT_VARIABLE device_api
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
    )
    if(NOT device_api MATCHES "^[0-9]+$")
        message(FATAL_ERROR "Invalid Android API returned by adb: ${device_api}")
    endif()
    if(device_api GREATER 34)
        set(device_api 34)
    endif()
    set(ANDROID_PLATFORM "android-${device_api}" CACHE STRING "Android target API")
endif()

project(airgap LANGUAGES C CXX)

set(glue "${CMAKE_ANDROID_NDK}/sources/android/native_app_glue")
add_library(airgap SHARED
    native/main.cpp
    native/bip39_words.cpp
    native/ui.cpp
    native/camera.cpp
    native/crypto/abi.c
    native/crypto/eip_1559_tx.c
    native/crypto/eth_crypto.c
    native/crypto/rlp.c
    native/crypto/selector.c
    "${glue}/android_native_app_glue.c"
)
target_include_directories(airgap PRIVATE "${glue}" native/crypto)
set_target_properties(airgap PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
target_compile_options(airgap PRIVATE -O2)
target_link_libraries(airgap PRIVATE android log EGL GLESv3 camera2ndk mediandk)

# BEGIN vcpkg-install: imgui
find_package(imgui REQUIRED)
if(NOT TARGET imgui::imgui)
    message(FATAL_ERROR "imgui did not define target imgui::imgui")
endif()
target_link_libraries(airgap PRIVATE imgui::imgui)
# END vcpkg-install: imgui

# BEGIN vcpkg-install: secp256k1
find_package(libsecp256k1 REQUIRED)
if(NOT TARGET libsecp256k1::secp256k1)
    message(FATAL_ERROR "libsecp256k1 did not define target libsecp256k1::secp256k1")
endif()
target_link_libraries(airgap PRIVATE libsecp256k1::secp256k1)
# END vcpkg-install: secp256k1

# BEGIN vcpkg-install: openssl
find_package(OpenSSL REQUIRED)
if(NOT TARGET OpenSSL::Crypto)
    message(FATAL_ERROR "OpenSSL did not define target OpenSSL::Crypto")
endif()
if(NOT TARGET OpenSSL::SSL)
    message(FATAL_ERROR "OpenSSL did not define target OpenSSL::SSL")
endif()
target_link_libraries(airgap PRIVATE OpenSSL::Crypto OpenSSL::SSL)
# END vcpkg-install: openssl
