
list(APPEND SOURCE
    smb2rdr.c
    upcall.c
    smb2rdr.h
    smb2rdr_ioctl.h)

add_definitions(-DRDBSS_TRACKER)

add_library(smb2rdr MODULE ${SOURCE})
set_module_type(smb2rdr kernelmodedriver)
target_link_libraries(smb2rdr ntoskrnl_vista rdbsslib rxce copysup memcmp)
add_importlibs(smb2rdr ntoskrnl hal)
add_cd_file(TARGET smb2rdr DESTINATION reactos/system32/drivers FOR all)
add_registry_inf(smb2rdr_reg.inf)
