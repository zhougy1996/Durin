include_guard(GLOBAL)

set(target_name Doge_glm)

set(GLM_ROOT "${DOGE_PROJECT_SOURCE_DIR}/ThirdParty/glm/glm")

add_library(${target_name} INTERFACE)

target_include_directories(${target_name} INTERFACE "${GLM_ROOT}")