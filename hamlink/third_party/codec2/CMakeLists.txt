# Trimmed Codec2 (speech codec only, no FreeDV/OFDM modems). LGPL-2.1, see COPYING.LGPL.
# Source: https://github.com/drowe67/codec2 @ 310777b (codebook*.c, config.h and version.h are the
# files its CMake generates on the host; checked in so the NDK build needs no host tools).
file(GLOB C2_SRCS ${CMAKE_CURRENT_LIST_DIR}/src/*.c)
add_library(codec2_lite STATIC ${C2_SRCS})
target_include_directories(codec2_lite PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src
                                              ${CMAKE_CURRENT_LIST_DIR}/src/codec2)
target_compile_definitions(codec2_lite PRIVATE CODEC2_VERSION_STR="1.2.0")
target_compile_options(codec2_lite PRIVATE -O2 -w)
set_target_properties(codec2_lite PROPERTIES POSITION_INDEPENDENT_CODE ON)
find_library(M_LIB m)
if(M_LIB)
  target_link_libraries(codec2_lite PUBLIC ${M_LIB})
endif()
