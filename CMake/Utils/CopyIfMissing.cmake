# Utility script: copy a file only when the destination does not already exist.

# Get source and destination paths from command line arguments
set(SRC "${CMAKE_ARGV3}")
set(DST "${CMAKE_ARGV4}")

if(EXISTS "${DST}")
    return()
endif()

get_filename_component(DST_DIR "${DST}" DIRECTORY)
file(MAKE_DIRECTORY "${DST_DIR}")
file(COPY "${SRC}" DESTINATION "${DST_DIR}")

get_filename_component(SRC_NAME "${SRC}" NAME)
if(NOT IS_DIRECTORY "${DST}")
    file(RENAME "${DST_DIR}/${SRC_NAME}" "${DST}")
endif()

message(STATUS "File copied: ${SRC} -> ${DST}")
