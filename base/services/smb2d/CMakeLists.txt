
list(APPEND SMB2_ENGINE_SOURCE
    smb2client.c
    smb2sign.c)

# Console test tool: exercises the protocol engine without the kernel redirector
add_executable(smb2cli smb2cli.c ${SMB2_ENGINE_SOURCE})
set_module_type(smb2cli win32cui UNICODE)
add_importlibs(smb2cli ws2_32 secur32 msvcrt kernel32)
add_cd_file(TARGET smb2cli DESTINATION reactos/system32 FOR all)
