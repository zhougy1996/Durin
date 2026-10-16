add_library(durin_httplib INTERFACE)
add_library(httplib::httplib ALIAS durin_httplib)
target_include_directories(durin_httplib SYSTEM INTERFACE
    ${DURIN_PROJECT_ROOT_DIR}/Source/ThirdParty/cpp-httplib)
target_compile_definitions(durin_httplib INTERFACE
    CPPHTTPLIB_REQUEST_URI_MAX_LENGTH=2048
    CPPHTTPLIB_HEADER_MAX_LENGTH=8192
    CPPHTTPLIB_HEADER_MAX_COUNT=64)
if(WIN32)
    target_link_libraries(durin_httplib INTERFACE ws2_32 crypt32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(durin_httplib INTERFACE Threads::Threads)
endif()
