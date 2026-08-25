add_durin_module(Engine)

# These implementations deliberately share private helper names with adjacent
# translation units.
set_source_files_properties(
    "Private/Materials/MaterialRenderTypes.cpp"
    "Private/Components/SkeletalMeshComponent.cpp"
    "Private/Texture/TextureCube.cpp"
    PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE
)
