list(APPEND SOURCE
    usbxhci.c
    ucxclient.c
    usbxhci.h
    tt.c)

add_library(usbxhci MODULE
    ${SOURCE}
    guid.c
    usbxhci.rc)

# The KMDF 1.33 client stub and the Ucx class-binding record are linked in, but
# the image entry point stays DriverEntry: the legacy usbport path never touches
# WDF; the UCX path (Parameters\UseUcx=1) enters the stub explicitly (ucxclient.c).
set_module_type(usbxhci kmdfdriver KMDF_VERSION 1.33 ENTRYPOINT DriverEntry 8)
target_include_directories(usbxhci PRIVATE ${REACTOS_SOURCE_DIR}/sdk/include/ddk/ucx/1.0)
target_link_libraries(usbxhci ucxstub)
add_importlibs(usbxhci usbport usbd hal ntoskrnl)
add_pch(usbxhci usbxhci.h SOURCE)
add_cd_file(TARGET usbxhci DESTINATION reactos/system32/drivers NO_CAB FOR all)
