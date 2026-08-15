durin_add_engine_functional_test(SplineTests
	KIND feature
	DOMAINS spline
	MODULES engine level-editor
	STACKS editor
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned spline editing white-box coverage avoids exporting private viewport and customization symbols."
	RUNTIME_STACK_RATIONALE "Exercises DurinEd spline customization behavior."
	SOURCES
		Private/SplineTests.cpp
		Private/SplineMeshComponentTests.cpp
		Private/SplineMeshDeformerTests.cpp
		Private/SplineV2ContractTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Customizations/SplineEditorCustomizations.cpp
		${_durin_level_editor_private}/Customizations/LevelEditorCustomizations.cpp
		${_durin_level_editor_private}/Viewport/ViewportCameraTransform.cpp
		${_durin_level_editor_private}/Viewport/LevelEditorViewportClient.cpp
		${_durin_level_editor_private}/Viewport/ViewportPickingService.cpp
		${_durin_level_editor_private}/Viewport/ViewportPickingSceneIndex.cpp
		${_durin_level_editor_private}/Viewport/LevelEditorViewportEditing.cpp
		${_durin_level_editor_private}/Viewport/TransformGizmo.cpp
		${_durin_level_editor_private}/Workspace/LevelEditorContext.cpp
	LIBRARIES ApplicationCore MonaCore Mona MonaImGui DurinEd AssetImportCore StandardAssetImport
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
)

durin_add_engine_functional_test(SplineQualificationTests
	KIND qualification
	DOMAINS spline
	MODULES engine
	SOURCES Private/SplineQualificationTests.cpp
)

durin_add_engine_functional_test(SkyBoxTests
	KIND feature
	DOMAINS sky-box
	MODULES engine geometry-build level-editor renderer standard-asset-import
	STACKS editor renderer
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned sky-box authoring white-box coverage avoids exporting private customization symbols."
	RUNTIME_STACK_RATIONALE "Exercises renderer-backed sky-box editing and rendering contracts."
	SOURCES
		Private/SkyBox/SkyBoxRenderingTests.cpp
		Private/SkyBox/SkyBoxComponentTests.cpp
		Private/SkyBox/SkyBoxEditorTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Authoring/SkyBoxLevelAuthoring.cpp
		${_durin_level_editor_private}/Customizations/SkyBoxDetails.cpp
		${_durin_level_editor_private}/Customizations/LevelEditorCustomizations.cpp
	LIBRARIES GeometryBuild StandardAssetImport RenderCore Renderer DurinEd
	DATA_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(SkyBoxVulkanIntegrationTests
	KIND integration
	DOMAINS sky-box
	MODULES engine geometry-build renderer standard-asset-import
	BACKENDS vulkan
	STACKS editor renderer
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE "Owns the Vulkan-backed sky-box integration lifecycle."
	RUNTIME_ONLY_RATIONALE "RHIInit selects VulkanRHI dynamically for this Vulkan-backed test."
	RUNTIME_ONLY_TARGETS VulkanRHI
	SOURCES Private/SkyBox/SkyBoxVulkanTests.cpp
	LIBRARIES ApplicationCore GeometryBuild StandardAssetImport RenderCore Renderer DurinEd
	DATA_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(RendererResourceReloadVulkanTests
	KIND integration
	DOMAINS renderer shader
	MODULES engine renderer
	BACKENDS vulkan
	STACKS renderer
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE "Exercises in-process renderer shader failure, reload, and recovery on Vulkan."
	RUNTIME_ONLY_RATIONALE "RHIInit selects VulkanRHI dynamically for this Vulkan-backed test."
	RUNTIME_ONLY_TARGETS VulkanRHI
	SOURCES Private/RendererResourceReloadVulkanTests.cpp
	LIBRARIES ApplicationCore RenderCore Renderer
)

durin_add_engine_functional_test(StaticMeshRenderPreparationVulkanTests
	KIND integration
	DOMAINS static-mesh
	MODULES engine renderer
	BACKENDS vulkan
	STACKS renderer
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE "Exercises view-local StaticMesh material preparation against initialized render resources."
	RUNTIME_ONLY_RATIONALE "RHIInit selects VulkanRHI dynamically for the prepared-section resource gate."
	RUNTIME_ONLY_TARGETS VulkanRHI
	SOURCES Private/StaticMeshRenderPreparationVulkanTests.cpp
	LIBRARIES ApplicationCore RenderCore Renderer
)

durin_add_engine_functional_test(DirectionalShadowBaselineVulkanTests
	KIND qualification
	DOMAINS renderer shadow
	MODULES engine renderer
	BACKENDS vulkan
	STACKS renderer
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE
		"Captures the frozen Q0 directional-shadow Lit baseline through the production Vulkan renderer."
	RUNTIME_ONLY_RATIONALE
		"RHIInit selects VulkanRHI dynamically for the hardware-backed baseline captures."
	RUNTIME_ONLY_TARGETS VulkanRHI
	SOURCES Private/DirectionalShadowBaselineVulkanTests.cpp
	LIBRARIES ApplicationCore RenderCore Renderer
	DATA_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Data/DirectionalShadowQ0
		${CMAKE_CURRENT_SOURCE_DIR}/Data/DirectionalShadowQ1
)

durin_add_engine_functional_test(HDRDisplayMappingQualificationTests
	KIND qualification
	DOMAINS renderer viewport
	MODULES engine renderer
	BACKENDS vulkan
	STACKS renderer
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE
		"Measures the production HDR copy and FXAA display routes at the frozen 1920x1080 RTX 3090 qualification point."
	RUNTIME_ONLY_RATIONALE
		"RHIInit selects VulkanRHI dynamically for hardware-backed timestamp measurements."
	RUNTIME_ONLY_TARGETS VulkanRHI
	SOURCES Private/HDRDisplayMappingQualificationTests.cpp
	LIBRARIES ApplicationCore RenderCore Renderer
	DATA_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Data/HDRDisplayMapping
)

durin_add_engine_functional_test(EditorRenderingTests
	KIND feature
	DOMAINS renderer
	MODULES durin-ed engine renderer
	STACKS editor renderer
	RUNTIME_STACK_RATIONALE "Exercises renderer-backed editor assistance and grid rendering."
	SOURCES
		Private/EditorGridRenderingTests.cpp
		Private/RendererEditorAssistanceTests.cpp
		Private/RendererResourceInvalidationTests.cpp
		Private/RendererResourceSlotCacheTests.cpp
		Private/RendererRenderTargetLayoutTests.cpp
		Private/RendererSceneViewTests.cpp
		Private/EditorTextureSmokeTests.cpp
	LIBRARIES ApplicationCore AssetImportCore StandardAssetImport RenderCore Renderer DurinEd MaterialEditor GeometryBuild
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(EditorGridVulkanTests
	KIND integration
	DOMAINS renderer viewport
	MODULES engine renderer vulkan-rhi
	BACKENDS vulkan
	STACKS editor renderer
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE
		"Exercises the production editor-grid shader and assistance pass through the Vulkan renderer."
	SOURCES Private/EditorGridVulkanTests.cpp
	LIBRARIES ApplicationCore RenderCore Renderer VulkanRHI Vulkan::Vulkan
	INCLUDE_DIRECTORIES
		${DURIN_PROJECT_SOURCE_DIR}/Runtime/VulkanRHI/Private
	COMPILE_DEFINITIONS DURIN_VULKAN_TEST_FAILURE_INJECTION=1
)

durin_add_engine_functional_test(EditorShellTests
	KIND feature
	DOMAINS editor-shell
	MODULES durin-ed level-editor
	STACKS editor
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned shell model white-box coverage avoids exporting private workspace and panel implementations."
	RUNTIME_STACK_RATIONALE "Exercises DurinEd and Mona editor-shell models."
	SOURCES
		Private/EditorBootstrapStateTests.cpp
		Private/EditorNotificationTests.cpp
		Private/EditorWorkspaceTests.cpp
		Private/UIStyleTests.cpp
		Private/ConsoleRecordModelTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Workspace/LevelEditorContext.cpp
		${_durin_level_editor_private}/Viewport/ViewportPickingSceneIndex.cpp
		${_durin_level_editor_private}/Panels/ConsoleRecordModel.cpp
	LIBRARIES ApplicationCore MonaCore Mona MonaImGui DurinEd
)

durin_add_engine_functional_test(ExternalToolTests
	KIND feature
	DOMAINS editor-shell
	MODULES durin-ed main-frame
	STACKS editor
	PRIVATE_SOURCE_OWNER MainFrame
	PRIVATE_SOURCE_RATIONALE
		"MainFrame-owned profiling integration white-box coverage avoids exporting the private service implementation."
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises the DurinEd profiling-tool integration."
	SOURCES Private/ProfilingToolServiceTests.cpp
	PRIVATE_SOURCES
		${_durin_main_frame_private}/ProfilingToolService.cpp
	LIBRARIES ApplicationCore MonaCore Mona MonaImGui DurinEd
)

# Cooked-runtime mode and Renderer/Vulkan teardown are process-global.
durin_add_engine_functional_test(TextureCookIntegrationTests
	KIND integration
	DOMAINS asset-cook texture
	MODULES engine geometry-build renderer standard-asset-import
	BACKENDS vulkan
	STACKS editor renderer
	EDITOR_ONLY
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE "Owns the renderer and Vulkan cooked-texture lifecycle."
	RUNTIME_ONLY_RATIONALE "RHIInit selects VulkanRHI dynamically for this Vulkan-backed test."
	RUNTIME_ONLY_TARGETS VulkanRHI
	SOURCES Private/Texture/TextureCookTests.cpp
	LIBRARIES GeometryBuild StandardAssetImport RenderCore Renderer
)
