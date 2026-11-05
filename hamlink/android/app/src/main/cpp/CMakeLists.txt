cmake_minimum_required(VERSION 3.18)
project(hamlink_jni C CXX)

# android/app/src/main/cpp -> repository root is five levels up
set(HL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)

# Reproducible native code: F-Droid rebuilds the APK in its own directories and compares it byte for byte with ours.
# Source and build paths are mapped to fixed names (debug info, __FILE__), and there is no build-id note (it hashes
# the unmapped object files).
get_filename_component(HL_ROOT_ABS ${HL_ROOT} ABSOLUTE)
add_compile_options(-ffile-prefix-map=${HL_ROOT_ABS}=hamlink -ffile-prefix-map=${CMAKE_BINARY_DIR}=build)
add_link_options(-Wl,--build-id=none)
add_subdirectory(${HL_ROOT}/third_party/codec2 codec2)
add_subdirectory(${HL_ROOT}/core core)

add_library(hamlink_jni SHARED jni_bridge.c)
# hamlink_api_static carries the AFSK + COFDMTV modems, the mesh layer, KISS/AX.25 and Codec2 voice.
target_link_libraries(hamlink_jni hamlink_api_static log)
