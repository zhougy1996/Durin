add_durin_module(Renderer)

# Preserve independent anonymous-namespace helpers used by adjacent renderer
# implementations.
set_source_files_properties(
    "Private/Renderers/EditorAssistance/OverlayIconRenderer.cpp"
    "Private/Renderers/EditorAssistance/OverlayLineRenderer.cpp"
    "Private/Renderers/SceneFrameExecutionPipeline.cpp"
    "Private/Renderers/SceneFramePostProcess.cpp"
    "Private/Renderers/VolumetricCloudShadowRenderer.cpp"
    "Private/Terrain/TerrainLOD.cpp"
    PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE
)
