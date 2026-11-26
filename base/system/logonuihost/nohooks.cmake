file(STRINGS "${MODULE}" Hits REGEX "LogonUiHostTest")
if(Hits)
    message(FATAL_ERROR "${MODULE} contains LogonUI host test hooks")
endif()
