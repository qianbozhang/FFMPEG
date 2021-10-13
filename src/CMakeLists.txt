SET(SRC_LIST  thubnail.cpp test.cpp)
#SET(SRC_LIST decode_to_yuv.cpp)

SET(EXECUTABLE_OUTPUT_PATH  ${PROJECT_SOURCE_DIR}/output/bin)
#SET(LIBRARY_OUTPUT_PATH  ${PROJECT_SOURCE_DIR}/output/lib)

#include & libs
INCLUDE_DIRECTORIES (${PUBLIC_INCLUDE_PATH})
LINK_DIRECTORIES (${PUBLIC_LIBS_PATH})


set (LIBS "-lavformat -lavcodec -lavutil -lswscale")

ADD_EXECUTABLE(Target ${SRC_LIST})
TARGET_LINK_LIBRARIES (Target ${LIBS})