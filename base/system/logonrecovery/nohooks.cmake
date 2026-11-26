file(STRINGS "${MODULE}" Hits REGEX "LogonRecoveryTest")
if(Hits)
    message(FATAL_ERROR "${MODULE} contains logon recovery test hooks")
endif()
