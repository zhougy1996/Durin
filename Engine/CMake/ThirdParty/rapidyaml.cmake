include_guard(GLOBAL)

include(FetchContent)

FetchContent_Declare(
    rapidyaml
    URL https://github.com/biojppm/rapidyaml/releases/download/v0.11.1/rapidyaml-0.11.1-src.zip
    URL_HASH SHA256=30054b74abdf0ba35bf2cb435b6e49fcb6d62a8e78a240a018c36aa60dba765f
    SOURCE_DIR "${DOGE_PROJECT_THIRD_PARTY_SOURCE_DIR}/rapidyaml"
)

set(CMAKE_MESSAGE_LOG_LEVEL NOTICE)
FetchContent_MakeAvailable(rapidyaml)
set(CMAKE_MESSAGE_LOG_LEVEL STATUS)