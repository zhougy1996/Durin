project(LevelEditor)

set(DHT_OUTPUT_DIRECTORY "${DOGE_INTERMEDIATE_DIR}/${PROJECT_NAME}/${DOGE_ARCH}/Include/DHT")

set(DHT_INPUT_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/Private/TestDHT.h
)

set(DHT_EXE "${DOGE_SCRIPT_DIR}/TestParse.py")

function (doge_set_dht_input_headers input_headers output_directory OUT_DHT_GENERATED_FILES)
	foreach(header ${input_headers})
		message("DHT Input Header: ${header}")
		get_filename_component(header_name ${header} NAME_WE)
		set(gen_cpp_file ${output_directory}/${header_name}.gen.cpp)
		set(gen_h_file ${output_directory}/${header_name}.gen.h)

		list(APPEND generated_files ${gen_cpp_file})
		list(APPEND generated_files ${gen_h_file})

		add_custom_command(
			OUTPUT ${gen_cpp_file} ${gen_h_file}
			COMMAND python ${DHT_EXE} ${header} ${output_directory}
			DEPENDS ${header}
			COMMENT "DHT Parsing ${header_name}.h"
		)
	endforeach()
	set(${OUT_DHT_GENERATED_FILES} ${generated_files} PARENT_SCOPE)
endfunction()

doge_set_dht_input_headers(${DHT_INPUT_HEADERS} ${DHT_OUTPUT_DIRECTORY} DHT_GENERATED_FILES)
message("DHT_GENERATED_FILES: ${DHT_GENERATED_FILES}")

doge_print_project_build_info()

doge_collect_and_organize_source_files(SRCS)

add_library(LevelEditor SHARED
	${SRCS}
	${DHT_GENERATED_FILES}
)

doge_setup_shared_library(LevelEditor)

target_link_libraries(LevelEditor PRIVATE
    Core
    RHI
    Engine
    ApplicationCore
    MonaCore
    Mona
    DogeEd
)


