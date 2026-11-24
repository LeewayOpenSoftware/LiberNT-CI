file(STRINGS "${MODULE}" Hits REGEX "CredProvsTest")
if(Hits)
    message(FATAL_ERROR "${MODULE} contains credential provider test hooks")
endif()
