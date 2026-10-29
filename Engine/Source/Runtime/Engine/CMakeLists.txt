add_durin_module(Engine)
target_link_libraries(Engine PRIVATE zstd::zstd)

target_include_directories(Engine PRIVATE "${CMAKE_SOURCE_DIR}/Engine/Source/Developer/DerivedDataCache/Private")
