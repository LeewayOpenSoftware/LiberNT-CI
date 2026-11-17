#
# PROJECT:     LiberNT USB host controller extension (UCX) -- M1 smoke test client
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     Build the M1 smoke client that binds the Ucx class library
# COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
#

add_library(ucxclient_test MODULE ucxclient_test.c)
set_module_type(ucxclient_test kmdfdriver)
target_include_directories(ucxclient_test PRIVATE ${REACTOS_SOURCE_DIR}/sdk/include/ddk/ucx/1.0)
target_link_libraries(ucxclient_test ucxstub ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(ucxclient_test ntoskrnl hal)
add_cd_file(TARGET ucxclient_test DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_registry_inf(ucxtest_reg.inf)
