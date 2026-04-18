include_guard(GLOBAL)

include(FetchContent)

FetchContent_Declare(
    glaze
    URL https://github.com/stephenberry/glaze/archive/refs/tags/v7.3.3.zip
    URL_HASH SHA256=894330b277273ecd636a879db7d714a9fa2ed43459327dff9aaf711bdbe9c9fe
    SOURCE_DIR "${DOGE_PROJECT_SOURCE_DIR}/ThirdParty/glaze"
)

FetchContent_MakeAvailable(glaze)
