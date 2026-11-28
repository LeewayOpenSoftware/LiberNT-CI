# fwsettings.exe: the elevated side of the Orosi Firewall page (shell32), named for the elevation prompt

file(GLOB fwsettings_rc_deps res/*.*)
add_rc_deps(fwsettings.rc ${fwsettings_rc_deps})
add_executable(fwsettings fwsettings.c fwsettings.rc)
set_module_type(fwsettings win32gui UNICODE)
add_importlibs(fwsettings msvcrt kernel32)
add_cd_file(TARGET fwsettings DESTINATION reactos/system32 FOR all)
