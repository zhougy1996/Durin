doge_add_project(Engine)

list(APPEND CMAKE_PREFIX_PATH "${DOGE_PROJECT_SOURCE_DIR}/ThirdParty")

# ThirdParty libraries
# Note: Keep this because ThirdParty libraries is not added via .dproject files
add_subdirectory(Source/ThirdParty)