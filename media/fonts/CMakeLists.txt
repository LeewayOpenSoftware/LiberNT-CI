
list(APPEND FONT_FILES
    arial.ttf
    arialbd.ttf
    arialbi.ttf
    ariali.ttf
    ariblk.ttf
    Caladea-Regular.ttf
    Caladea-Bold.ttf
    Caladea-Italic.ttf
    Caladea-BoldItalic.ttf
    Carlito-Regular.ttf
    Carlito-Bold.ttf
    Carlito-Italic.ttf
    Carlito-BoldItalic.ttf
    CascadiaMono-Regular.ttf
    CascadiaMono-Bold.ttf
    CascadiaMono-Italic.ttf
    CascadiaMono-BoldItalic.ttf
    comic.ttf
    comicbd.ttf
    cour.ttf
    courbd.ttf
    courbi.ttf
    couri.ttf
    framd.ttf
    framdit.ttf
    FreeSans.ttf
    FreeSansBold.ttf
    FreeSansBoldOblique.ttf
    FreeSansOblique.ttf
    FSEX301.ttf
    FSEX301-L2.ttf
    georgia.ttf
    georgiab.ttf
    georgiai.ttf
    georgiaz.ttf
    lucon.ttf
    Marlett.ttf
    micross.ttf
    NotoSansJP-Regular.otf
    NotoSansSC-Regular.otf
    pala.ttf
    palab.ttf
    palabi.ttf
    palai.ttf
    SourceSansPro-Regular.ttf
    SourceSansPro-Bold.ttf
    SourceSansPro-BoldIt.ttf
    SourceSansPro-It.ttf
    #ssee874.fon # to be added (CORE-16165 and CORE-16166)
    #ssee1255.fon # to be added (CORE-16165 and CORE-16166)
    #ssee1256.fon # to be added (CORE-16165 and CORE-16166)
    #ssee1257.fon # to be added (CORE-16165 and CORE-16166)
    #sserife.fon # to be added (CORE-16165 and CORE-16166)
    #sserifeg.fon # to be added (CORE-16165 and CORE-16166)
    #sserifer.fon # to be added (CORE-16165 and CORE-16166)
    #sserifet.fon # to be added (CORE-16165 and CORE-16166)
    segmdl2.ttf
    selawk.ttf
    selawkb.ttf
    selawkl.ttf
    selawksb.ttf
    selawksl.ttf
    symbol.ttf
    tahoma.ttf
    tahomabd.ttf
    times.ttf
    timesbd.ttf
    timesbi.ttf
    timesi.ttf
    trebuc.ttf
    trebucbd.ttf
    trebucbi.ttf
    trebucit.ttf
    UniVGA16.ttf
    verdanab.ttf
    verdanaz.ttf
    verdanai.ttf
    verdana.ttf
    #vgaoem.fon # to be added (CORE-17327)
    wingding.ttf)

foreach(item ${FONT_FILES})
    add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/${item}" DESTINATION reactos/Fonts FOR all)
endforeach(item)

# With the Fonts key filled, win32k loads fonts from it and skips the directory scan, so every shipped file needs a value
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${REACTOS_SOURCE_DIR}/boot/bootdata/hivesft.inf")
file(READ "${REACTOS_SOURCE_DIR}/boot/bootdata/hivesft.inf" _hivesft_inf)
set(_fonts_missing "")
foreach(item ${FONT_FILES})
    string(REPLACE "." "\\." _item_re "${item}")
    if(NOT _hivesft_inf MATCHES "Fonts\",\"[^\"\r\n]*\",0x00000000,\"${_item_re}\"")
        list(APPEND _fonts_missing "${item}")
    endif()
endforeach()
if(_fonts_missing)
    message(FATAL_ERROR "No Fonts registry value in boot/bootdata/hivesft.inf for: ${_fonts_missing}")
endif()
unset(_hivesft_inf)

add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/doc/Carlito/OFL.txt"
    DESTINATION reactos NAME_ON_CD Carlito-LICENSE.txt NO_CAB FOR all)
add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/doc/Carlito/README.txt"
    DESTINATION reactos NAME_ON_CD Carlito-README.txt NO_CAB FOR all)
add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/doc/Caladea/LICENSE.txt"
    DESTINATION reactos NAME_ON_CD Caladea-LICENSE.txt NO_CAB FOR all)
add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/doc/Caladea/README.txt"
    DESTINATION reactos NAME_ON_CD Caladea-README.txt NO_CAB FOR all)
add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/doc/CascadiaMono/OFL.txt"
    DESTINATION reactos NAME_ON_CD CascadiaMono-LICENSE.txt NO_CAB FOR all)
add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/doc/CascadiaMono/README.txt"
    DESTINATION reactos NAME_ON_CD CascadiaMono-README.txt NO_CAB FOR all)
add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/doc/NotoSansJP/LICENSE.txt"
    DESTINATION reactos NAME_ON_CD NotoSansJP-LICENSE.txt NO_CAB FOR all)
add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/doc/NotoSansSC/LICENSE.txt"
    DESTINATION reactos NAME_ON_CD NotoSansSC-LICENSE.txt NO_CAB FOR all)
