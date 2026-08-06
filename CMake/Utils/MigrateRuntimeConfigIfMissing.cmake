# Preserve an existing runtime config while moving it into Saved/Configs.

set(TEMPLATE_FILE "${CMAKE_ARGV3}")
set(LEGACY_FILE "${CMAKE_ARGV4}")
set(DESTINATION_FILE "${CMAKE_ARGV5}")

if(EXISTS "${DESTINATION_FILE}")
	return()
endif()

get_filename_component(DESTINATION_DIRECTORY "${DESTINATION_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${DESTINATION_DIRECTORY}")

if(EXISTS "${LEGACY_FILE}")
	file(RENAME "${LEGACY_FILE}" "${DESTINATION_FILE}")
	message(STATUS "Runtime config migrated: ${LEGACY_FILE} -> ${DESTINATION_FILE}")
	return()
endif()

file(COPY_FILE "${TEMPLATE_FILE}" "${DESTINATION_FILE}")
message(STATUS "Runtime config created: ${TEMPLATE_FILE} -> ${DESTINATION_FILE}")
