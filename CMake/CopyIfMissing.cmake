set(SRC "${CMAKE_ARGV3}")
set(DST "${CMAKE_ARGV4}")

if(NOT EXISTS "${DST}")
    file(COPY "${SRC}" DESTINATION "${DST}")
endif()