
spec2def(smb2np.dll smb2np.spec ADD_IMPORTLIB)

include_directories(${REACTOS_SOURCE_DIR}/drivers/filesystems/smb2rdr)

add_library(smb2np MODULE
    smb2np.c
    smb2np.rc
    ${CMAKE_CURRENT_BINARY_DIR}/smb2np.def)

set_module_type(smb2np win32dll UNICODE)
add_importlibs(smb2np msvcrt kernel32)
add_cd_file(TARGET smb2np DESTINATION reactos/system32 FOR all)
