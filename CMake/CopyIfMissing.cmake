# Get source and destination paths from command line arguments
set(SRC "${CMAKE_ARGV3}")
set(DST "${CMAKE_ARGV4}")

# If destination already exists, exit early (no need to copy)
if(EXISTS "${DST}")
    return()
endif()

# Extract directory path of the destination
get_filename_component(DST_DIR "${DST}" DIRECTORY)

# Create the destination directory if it does not exist
file(MAKE_DIRECTORY "${DST_DIR}")

# First copy the source file to the destination directory
file(COPY "${SRC}" DESTINATION "${DST_DIR}")

# Get the original filename of the source
get_filename_component(SRC_NAME "${SRC}" NAME)

# If destination is a file path (not a directory), rename the copied file
if(NOT IS_DIRECTORY "${DST}")
    file(RENAME "${DST_DIR}/${SRC_NAME}" "${DST}")
endif()

message(STATUS "File copied: ${SRC} -> ${DST}")