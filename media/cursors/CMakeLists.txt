# Orosi Default pointer scheme: the user32 pointer resources installed as files under
# %SystemRoot%\Cursors, so file-based schemes (HKLM Cursors\Schemes, the Mouse CPL preview
# and browse dialog) have the same art as the built-in pointers. One multi-size file per
# pointer; the size follows CursorBaseSize, so no large/extra-large copies are needed.
set(OROSI_CURSOR_SOURCE ${REACTOS_SOURCE_DIR}/win32ss/user/user32/resources)

macro(add_orosi_cursor _source _name)
    add_cd_file(FILE "${OROSI_CURSOR_SOURCE}/${_source}" DESTINATION reactos/Cursors NAME_ON_CD ${_name} FOR all)
endmacro()

add_orosi_cursor(ocr_normal.cur orosi_arrow.cur)
add_orosi_cursor(ocr_help.cur orosi_helpsel.cur)
add_orosi_cursor(ocr_appstarting.ani orosi_busy.ani)
add_orosi_cursor(ocr_wait.ani orosi_wait.ani)
add_orosi_cursor(ocr_cross.cur orosi_cross.cur)
add_orosi_cursor(ocr_ibeam.cur orosi_ibeam.cur)
add_orosi_cursor(ocr_pen.cur orosi_pen.cur)
add_orosi_cursor(ocr_no.cur orosi_unavail.cur)
add_orosi_cursor(ocr_sizens.cur orosi_ns.cur)
add_orosi_cursor(ocr_sizewe.cur orosi_ew.cur)
add_orosi_cursor(ocr_sizenwse.cur orosi_nwse.cur)
add_orosi_cursor(ocr_sizenesw.cur orosi_nesw.cur)
add_orosi_cursor(ocr_sizeall.cur orosi_move.cur)
add_orosi_cursor(ocr_up.cur orosi_up.cur)
add_orosi_cursor(ocr_hand.cur orosi_link.cur)
