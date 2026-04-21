include(FetchContent)

FetchContent_Declare(
    rapidyaml
    URL https://github.com/biojppm/rapidyaml/releases/download/v0.11.1/rapidyaml-0.11.1-src.zip
    URL_HASH SHA256=30054b74abdf0ba35bf2cb435b6e49fcb6d62a8e78a240a018c36aa60dba765f
)

if(MSVC)
    add_compile_options(/utf-8)
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RAPIDYAML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(rapidyaml)

