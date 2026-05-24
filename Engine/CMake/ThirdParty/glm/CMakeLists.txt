include(FetchContent)

FetchContent_Declare(
    glm
    URL https://github.com/g-truc/glm/archive/refs/tags/1.0.3.zip
    URL_HASH SHA256=3aa4347b8f13cba882df1c7b61a6ca910c75a875c56ec3d75d7dc9ae8eac34df
)

set(GLM_QUIET OFF CACHE BOOL "" FORCE)
set(GLM_TEST_ENABLE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glm)