
add_executable(netcxtest netcxtest.c)
set_module_type(netcxtest win32cui UNICODE)
add_importlibs(netcxtest iphlpapi setupapi cfgmgr32 newdev advapi32 ws2_32 msvcrt kernel32 ntdll)
