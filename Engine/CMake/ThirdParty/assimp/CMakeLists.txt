include(FetchContent)

FetchContent_Declare(
    assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG        v6.0.4
    GIT_PROGRESS   ON
    GIT_SHALLOW    ON
)

# Build as shared library
set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)

set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "")
set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE BOOL "") # .obj
set(ASSIMP_BUILD_FBX_IMPORTER ON CACHE BOOL "") # .fbx
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "") # .gltf, .glb
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "")
set(ASSIMP_INSTALL OFF CACHE BOOL "")
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "")
set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "Disable Assimp samples" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "")
set(ASSIMP_INJECT_DEBUG_POSTFIX OFF CACHE BOOL "")
set(ASSIMP_INSTALL_PDB OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(assimp)

get_target_property(lib_type assimp::assimp TYPE)
message(STATUS "assimp type is: ${lib_type}")
