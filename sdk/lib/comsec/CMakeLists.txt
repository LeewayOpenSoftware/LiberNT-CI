# SPDX-License-Identifier: MIT
add_library(comsec STATIC comsec.c)
target_include_directories(comsec PUBLIC ${REACTOS_SOURCE_DIR}/sdk/include/reactos/libs/comsec)
add_dependencies(comsec psdk)
