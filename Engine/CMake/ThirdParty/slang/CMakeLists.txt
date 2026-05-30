include(${CMAKE_CURRENT_LIST_DIR}/../DurinThirdParty.cmake)

add_library(Slang_Imported SHARED IMPORTED GLOBAL)

set(SLANG_ROOT "${DURIN_PROJECT_DIR}/External/Packages/slang")
set(SLANG_INCLUDE_DIR "${SLANG_ROOT}/include")

if(WIN32)
	durin_third_party_find_single_file(SLANG_RUNTIME "${SLANG_ROOT}/bin"
		"slang.dll"
	)
	durin_third_party_find_single_file(SLANG_IMPORT_LIB "${SLANG_ROOT}/lib"
		"slang.lib"
	)
	set_target_properties(Slang_Imported PROPERTIES
		IMPORTED_LOCATION "${SLANG_RUNTIME}"
		IMPORTED_IMPLIB "${SLANG_IMPORT_LIB}"
		INTERFACE_INCLUDE_DIRECTORIES "${SLANG_INCLUDE_DIR}"
	)
elseif(APPLE)
	durin_third_party_find_single_file(SLANG_RUNTIME "${SLANG_ROOT}/lib"
		"libslang.dylib"
	)
	set_target_properties(Slang_Imported PROPERTIES
		IMPORTED_LOCATION "${SLANG_RUNTIME}"
		INTERFACE_INCLUDE_DIRECTORIES "${SLANG_INCLUDE_DIR}"
	)
else()
	durin_third_party_find_single_file(SLANG_RUNTIME "${SLANG_ROOT}/lib"
		"libslang.so"
		"libslang.so.0"
	)
	set_target_properties(Slang_Imported PROPERTIES
		IMPORTED_LOCATION "${SLANG_RUNTIME}"
		INTERFACE_INCLUDE_DIRECTORIES "${SLANG_INCLUDE_DIR}"
	)
endif()

if(NOT EXISTS "${SLANG_INCLUDE_DIR}")
	message(FATAL_ERROR
		"Slang headers were not found at ${SLANG_INCLUDE_DIR}.\n"
		"Prepare slang first with Engine/Scripts/Bootstrap/setup_third_party.py --libs slang."
	)
endif()

if(NOT SLANG_RUNTIME OR NOT EXISTS "${SLANG_RUNTIME}")
	message(FATAL_ERROR
		"Slang runtime library was not found under ${SLANG_ROOT}.\n"
		"Prepare slang first with Engine/Scripts/Bootstrap/setup_third_party.py --libs slang."
	)
endif()

if(WIN32 AND (NOT SLANG_IMPORT_LIB OR NOT EXISTS "${SLANG_IMPORT_LIB}"))
	message(FATAL_ERROR
		"Slang import library was not found under ${SLANG_ROOT}.\n"
		"Prepare slang first with Engine/Scripts/Bootstrap/setup_third_party.py --libs slang."
	)
endif()
