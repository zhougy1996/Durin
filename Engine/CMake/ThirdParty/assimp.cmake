include_guard(GLOBAL)

include(FetchContent)

FetchContent_Declare(
    assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG        v6.0.4
    GIT_PROGRESS   ON
    GIT_SHALLOW    ON
)

set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "")
set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE BOOL "") # .obj
set(ASSIMP_BUILD_FBX_IMPORTER ON CACHE BOOL "") # .fbx
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "") # .gltf, .glb
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "")
set(ASSIMP_INSTALL OFF CACHE BOOL "")
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "")
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "")

FetchContent_MakeAvailable(assimp)

if(TARGET assimp)
    # Remove the /source-charset:utf-8 flag, which conflicts with the /utf-8 flag used by our project.
    get_target_property(ASSIMP_FLAGS assimp COMPILE_OPTIONS)
    if(ASSIMP_FLAGS)
        list(REMOVE_ITEM ASSIMP_FLAGS "/source-charset:utf-8")
        set_target_properties(assimp PROPERTIES COMPILE_OPTIONS "${ASSIMP_FLAGS}")
    endif()
endif()