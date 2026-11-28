# Orosi desktop themes for Personalization, installed where Windows keeps them
foreach(theme Orosi-Blue Orosi-Green Orosi-Dark)
    add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/${theme}.theme" DESTINATION reactos/Resources/Themes FOR all)
endforeach()
