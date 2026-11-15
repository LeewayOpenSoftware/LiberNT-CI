# LiberNT desktop themes for Personalization, installed where Windows keeps them
foreach(theme LiberNT-Blue LiberNT-Green LiberNT-Dark)
    add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/${theme}.theme" DESTINATION reactos/Resources/Themes FOR all)
endforeach()
