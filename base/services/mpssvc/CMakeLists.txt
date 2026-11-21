add_library(mpssvc_policy OBJECT policy.c filter.c)

# the rule parser alone, shared with hnetcfg (INetFwPolicy2)
add_library(mpssvc_rules OBJECT policy.c)
target_include_directories(mpssvc_rules PRIVATE ${REACTOS_SOURCE_DIR}/sdk/include)
add_dependencies(mpssvc_rules psdk)
target_include_directories(mpssvc_policy PRIVATE ${REACTOS_SOURCE_DIR}/sdk/include)
add_dependencies(mpssvc_policy psdk)

add_executable(mpssvc mpssvc.c $<TARGET_OBJECTS:mpssvc_policy>)
set_module_type(mpssvc win32cui UNICODE)
add_importlibs(mpssvc fwpuclnt iphlpapi ole32 advapi32 msvcrt kernel32)
add_cd_file(TARGET mpssvc DESTINATION reactos/system32 FOR all)
