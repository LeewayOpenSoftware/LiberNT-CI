
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
    NotoEmoji.ttf
    NotoSansDevanagari-Regular.ttf
    NotoSansJP-Bold.otf
    NotoSansJP-Regular.otf
    NotoSansKR-Bold.otf
    NotoSansKR-Regular.otf
    NotoSansSC-Bold.otf
    NotoSansTC-Bold.otf
    NotoSansTC-Regular.otf
    NotoSansThai-Regular.ttf
    NotoSansSC-Regular.otf
    pala.ttf
    palab.ttf
    palabi.ttf
    palai.ttf
    SourceSansPro-Regular.ttf
    SourceSansPro-Bold.ttf
    SourceSansPro-BoldIt.ttf
    SourceSansPro-It.ttf
    ssee874.fon
    ssee1255.fon
    ssee1256.fon
    ssee1257.fon
    sserife.fon
    sserifeg.fon
    sserifer.fon
    sserifet.fon
    segmdl2.ttf
    SegoeIcons.ttf
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
    TwemojiMozilla.ttf
    UniVGA16.ttf
    verdanab.ttf
    verdanaz.ttf
    verdanai.ttf
    verdana.ttf
    vgaoem.fon
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
add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/doc/SegoeMDL2Assets/LICENSE-MIT.txt"
    DESTINATION reactos NAME_ON_CD SegoeMDL2Assets-LICENSE.txt NO_CAB FOR all)
add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/doc/SegoeMDL2Assets/README.txt"
    DESTINATION reactos NAME_ON_CD SegoeMDL2Assets-README.txt NO_CAB FOR all)
add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/doc/SegoeFluentIcons/LICENSE-MIT.txt"
    DESTINATION reactos NAME_ON_CD SegoeFluentIcons-LICENSE.txt NO_CAB FOR all)
add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/doc/SegoeFluentIcons/README.txt"
    DESTINATION reactos NAME_ON_CD SegoeFluentIcons-README.txt NO_CAB FOR all)
add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/doc/NotoSansKR/LICENSE.txt"
    DESTINATION reactos NAME_ON_CD NotoSansKR-LICENSE.txt NO_CAB FOR all)
add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/doc/NotoSansTC/LICENSE.txt"
    DESTINATION reactos NAME_ON_CD NotoSansTC-LICENSE.txt NO_CAB FOR all)
add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/doc/NotoSansThai/LICENSE.txt"
    DESTINATION reactos NAME_ON_CD NotoSansThai-LICENSE.txt NO_CAB FOR all)
add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/doc/NotoSansDevanagari/LICENSE.txt"
    DESTINATION reactos NAME_ON_CD NotoSansDevanagari-LICENSE.txt NO_CAB FOR all)

# Licence texts and the provenance table of every shipped font go on the media next to the fonts
add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/doc/PROVENANCE.md"
    DESTINATION reactos/licenses/fonts NAME_ON_CD PROVENANCE.md NO_CAB FOR all)
file(GLOB FONT_DOC_DIRS LIST_DIRECTORIES true CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/doc/*")
foreach(font_doc_dir ${FONT_DOC_DIRS})
    if(IS_DIRECTORY "${font_doc_dir}")
        get_filename_component(font_family "${font_doc_dir}" NAME)
        file(GLOB font_doc_files CONFIGURE_DEPENDS "${font_doc_dir}/*")
        foreach(font_doc_file ${font_doc_files})
            get_filename_component(font_doc_name "${font_doc_file}" NAME)
            string(REPLACE " " "_" font_doc_cd_name "${font_family}-${font_doc_name}")
            add_cd_file(FILE "${font_doc_file}"
                DESTINATION reactos/licenses/fonts NAME_ON_CD "${font_doc_cd_name}" NO_CAB FOR all)
        endforeach()
    endif()
endforeach()
