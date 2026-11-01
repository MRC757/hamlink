cmake_minimum_required(VERSION 3.18)
project(hamlink_jni C CXX)

# android/app/src/main/cpp -> repository root is five levels up
set(HL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)
add_subdirectory(${HL_ROOT}/third_party/codec2 codec2)
add_subdirectory(${HL_ROOT}/core core)

add_library(hamlink_jni SHARED jni_bridge.c)
# hamlink_api_static carries the AFSK + COFDMTV modems, the mesh layer, KISS/AX.25 and Codec2 voice.
target_link_libraries(hamlink_jni hamlink_api_static log)
