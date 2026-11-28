spec2def(kernel.appcore.dll kernel.appcore.spec ADD_IMPORTLIB)

add_library(kernel_appcore MODULE
    runtime.c
    ${CMAKE_CURRENT_BINARY_DIR}/kernel.appcore.def)

set_target_properties(kernel_appcore PROPERTIES OUTPUT_NAME "kernel.appcore")
target_compile_definitions(kernel_appcore PRIVATE _KERNEL32_)
set_module_type(kernel_appcore win32dll ENTRYPOINT 0)
add_importlibs(kernel_appcore kernelbase kernel32 ntdll)
add_cd_file(TARGET kernel_appcore DESTINATION reactos/system32 FOR all)
