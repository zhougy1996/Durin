add_library(Slang_Imported SHARED IMPORTED GLOBAL)

set(SLANG_ROOT "${DOGE_PROJECT_DIR}/Source/ThirdParty/slang")

set_target_properties(Slang_Imported PROPERTIES
    IMPORTED_LOCATION "${SLANG_ROOT}/bin/slang.dll"
    IMPORTED_IMPLIB   "${SLANG_ROOT}/lib/slang.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${SLANG_ROOT}/include"
)

get_target_property(SLANG_TYPE Slang_Imported TYPE)
message(STATUS "slang type is: ${SLANG_TYPE}")