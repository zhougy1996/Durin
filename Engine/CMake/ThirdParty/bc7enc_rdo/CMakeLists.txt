cmake_minimum_required(VERSION 3.24)

get_filename_component(durin_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(bc7enc_rdo_source_dir "${durin_root}/Engine/External/Source/bc7enc_rdo")

foreach(required_file LICENSE bc7decomp.cpp bc7decomp.h bc7enc.cpp bc7enc.h rgbcx.cpp rgbcx.h)
	if(NOT EXISTS "${bc7enc_rdo_source_dir}/${required_file}")
		message(FATAL_ERROR
			"bc7enc_rdo source is incomplete at ${bc7enc_rdo_source_dir}.\n"
			"Run DevTool.bat dependency prepare --libs bc7enc_rdo first."
		)
	endif()
endforeach()

add_library(bc7enc_rdo STATIC
	"${bc7enc_rdo_source_dir}/bc7decomp.cpp"
	"${bc7enc_rdo_source_dir}/bc7enc.cpp"
	"${bc7enc_rdo_source_dir}/rgbcx.cpp"
)
add_library(bc7enc_rdo::bc7enc_rdo ALIAS bc7enc_rdo)

target_include_directories(bc7enc_rdo PUBLIC "${bc7enc_rdo_source_dir}")
set_target_properties(bc7enc_rdo PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
	target_compile_options(bc7enc_rdo PRIVATE /W0)
else()
	target_compile_options(bc7enc_rdo PRIVATE -w -fno-strict-aliasing)
endif()
