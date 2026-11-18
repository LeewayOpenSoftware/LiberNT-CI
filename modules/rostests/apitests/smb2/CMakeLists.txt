
include_directories(${REACTOS_SOURCE_DIR}/base/services/smb2d)

list(APPEND SOURCE
    Smb2Crypto.c
    Smb2Lock.c
    Smb2Notify.c
    Smb2Rename.c
    testlist.c
    ${REACTOS_SOURCE_DIR}/base/services/smb2d/smb2crypto.c
    ${REACTOS_SOURCE_DIR}/base/services/smb2d/smb2enc.c
    ${REACTOS_SOURCE_DIR}/base/services/smb2d/smb2sign.c)

add_executable(smb2_apitest ${SOURCE})
set_module_type(smb2_apitest win32cui)
add_importlibs(smb2_apitest msvcrt kernel32)
add_rostests_file(TARGET smb2_apitest)
