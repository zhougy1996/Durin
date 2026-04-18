include_guard(GLOBAL)

include(FetchContent)

FetchContent_Declare(
    glm
    URL https://github.com/g-truc/glm/archive/refs/tags/1.0.3.zip
    URL_HASH SHA256=3aa4347b8f13cba882df1c7b61a6ca910c75a875c56ec3d75d7dc9ae8eac34df
    SOURCE_DIR "${DOGE_PROJECT_SOURCE_DIR}/ThirdParty/glm"
)

FetchContent_MakeAvailable(glm)