set_source_files_properties(
    ${DURIN_PROJECT_DIR}/Source/ThirdParty/yyjson/yyjson.c
    PROPERTIES LANGUAGE CXX
)

add_library(yyjson STATIC
    ${DURIN_PROJECT_DIR}/Source/ThirdParty/yyjson/yyjson.c
)

add_library(yyjson::yyjson ALIAS yyjson)

target_include_directories(yyjson PUBLIC
    ${DURIN_PROJECT_DIR}/Source/ThirdParty/yyjson
)

if (MSVC)
    target_compile_options(yyjson PRIVATE /W0)
endif()
