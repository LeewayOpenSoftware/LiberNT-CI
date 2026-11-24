# test_sihost.exe - Apitest for sihost.exe
# CMake build configuration (ReactOS-shaped apitests)

list(APPEND APITEST_SOURCE_APITEST
    test_sihost.cpp
    testlist.c
)

add_apitest(test_sihost)
