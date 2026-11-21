
list(APPEND FONT_FILES
    arial.ttf
    arialbd.ttf
    arialbi.ttf
    ariali.ttf
    ariblk.ttf
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

add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/doc/NotoSansJP/LICENSE.txt"
    DESTINATION reactos NAME_ON_CD NotoSansJP-LICENSE.txt NO_CAB FOR all)
add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/doc/NotoSansSC/LICENSE.txt"
    DESTINATION reactos NAME_ON_CD NotoSansSC-LICENSE.txt NO_CAB FOR all)

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
