
list(APPEND SMB2_ENGINE_SOURCE
    smb2client.c
    smb2crypto.c
    smb2enc.c
    smb2sign.c)

# Console test tool: exercises the protocol engine without the kernel redirector
add_executable(smb2cli smb2cli.c ${SMB2_ENGINE_SOURCE})
set_module_type(smb2cli win32cui UNICODE)
add_importlibs(smb2cli ws2_32 secur32 msvcrt kernel32)
add_cd_file(TARGET smb2cli DESTINATION reactos/system32 FOR all)

# Daemon serving the smb2rdr kernel mini-redirector
include_directories(${REACTOS_SOURCE_DIR}/drivers/filesystems/smb2rdr)
add_executable(smb2d smb2d.c ${SMB2_ENGINE_SOURCE})
set_module_type(smb2d win32cui)
add_importlibs(smb2d ws2_32 secur32 advapi32 msvcrt kernel32)
add_cd_file(TARGET smb2d DESTINATION reactos/system32 FOR all)
add_registry_inf(smb2d_reg.inf)
