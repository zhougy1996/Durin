durin_add_native_test(SplineTests
	KIND feature
	DOMAINS spline
	MODULES asset-tools engine level-editor static-mesh-build asset-forge-builtins
	STACKS editor
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned spline editing white-box coverage avoids exporting private viewport and customization symbols."
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
	LIBRARIES
		Core
		CoreDObject
		Engine
		ApplicationCore
		MonaCore
		Mona
		MonaImGui
		AssetTools
		DurinEd
		StaticMeshBuild
		AssetForgeBuiltins
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${_durin_level_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
	COMPILE_DEFINITIONS LEVELEDITOR_EXPORTS
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	HEAVY_RUNTIME_RATIONALE "Exercises DurinEd spline customization behavior."
)

durin_add_native_test(SplineQualificationTests
	KIND qualification
	DOMAINS spline
	MODULES engine
	SOURCES Private/SplineQualificationTests.cpp
	LIBRARIES Core CoreDObject Engine
)

durin_add_native_test(SkyBoxTests
	KIND feature
	DOMAINS sky-box
	MODULES asset-tools engine static-mesh-build level-editor renderer asset-forge-builtins
	STACKS editor renderer
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned sky-box placement white-box coverage avoids exporting private customization symbols."
	SOURCES
		Private/SkyBox/SkyBoxRenderingTests.cpp
		Private/SkyBox/SkyBoxComponentTests.cpp
		Private/SkyBox/SkyBoxEditorTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Operations/SkyBoxPlacement.cpp
		${_durin_level_editor_private}/Customizations/LevelEditorCustomizations.cpp
	LIBRARIES Core CoreDObject Engine AssetTools StaticMeshBuild AssetForgeBuiltins RenderCore Renderer DurinEd
	DATA_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/Data
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${_durin_level_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
	COMPILE_DEFINITIONS LEVELEDITOR_EXPORTS
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	HEAVY_RUNTIME_RATIONALE "Exercises renderer-backed sky-box editing and rendering contracts."
)

durin_add_native_test(SkyBoxVulkanIntegrationTests
	KIND ${_durin_vulkan_integration_kind}
	DOMAINS sky-box
	MODULES asset-tools engine static-mesh-build texture-build renderer asset-forge-builtins
	BACKENDS vulkan
	STACKS editor renderer
	TIMEOUT 900
	RUNTIME_ONLY_RATIONALE "RHIInit selects VulkanRHI dynamically for this Vulkan-backed test."
	RUNTIME_ONLY_TARGETS VulkanRHI
	SOURCES Private/SkyBox/SkyBoxVulkanTests.cpp
	LIBRARIES
		Core
		CoreDObject
		Engine
		ApplicationCore
		AssetTools
		StaticMeshBuild
		TextureBuild
		AssetForgeBuiltins
		RenderCore
		Renderer
		DurinEd
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		${DURIN_PROJECT_ROOT_DIR}/Source/Developer/TextureBuild/Private
	DATA_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/Data
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	RESOURCE_LOCKS durin-gpu durin-rhi-lifecycle
	HEAVY_RUNTIME_RATIONALE "Owns the Vulkan-backed sky-box integration lifecycle."
)

durin_add_native_test(VolumetricCloudSceneContractTests
	KIND contract
	DOMAINS renderer volumetric-cloud
	MODULES engine renderer
	STACKS renderer
	SOURCES Private/VolumetricCloudSceneContractTests.cpp
	LIBRARIES Core CoreDObject Engine RenderCore Renderer
	INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
	ENVIRONMENTS authored-shaders
	HEAVY_RUNTIME_RATIONALE
		"Exercises the renderer-owned scene registry and pure P1 cloud translation without GPU initialization."
)

durin_add_native_test(RendererResourceReloadVulkanTests
	KIND ${_durin_vulkan_integration_kind}
	DOMAINS renderer shader
	MODULES engine renderer
	BACKENDS vulkan
	STACKS renderer
	TIMEOUT 900
	SOURCES Private/RendererResourceReloadVulkanTests.cpp
	LIBRARIES
		Core
		CoreDObject
		Engine
		ApplicationCore
		DerivedDataCache
		RenderCore
		Renderer
		ShaderBuild
		Slang_Imported
		VulkanRHI
		Vulkan::Vulkan
	INCLUDE_DIRECTORIES
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		${DURIN_PROJECT_SOURCE_DIR}/Runtime/VulkanRHI/Private
	COMPILE_DEFINITIONS DURIN_VULKAN_TEST_FAILURE_INJECTION=1
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	RESOURCE_LOCKS durin-gpu durin-rhi-lifecycle
	HEAVY_RUNTIME_RATIONALE
		"Exercises renderer shader recovery and resource-pool reuse with controlled native compute completion."
)

durin_add_native_test(StaticMeshRenderPreparationVulkanTests
	KIND ${_durin_vulkan_integration_kind}
	DOMAINS static-mesh
	MODULES engine renderer vulkan-rhi
	BACKENDS vulkan
	STACKS renderer
	TIMEOUT 900
	SOURCES Private/StaticMeshRenderPreparationVulkanTests.cpp
	LIBRARIES Core CoreDObject Engine ApplicationCore RenderCore Renderer ShaderBuild VulkanRHI Vulkan::Vulkan
	INCLUDE_DIRECTORIES
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		${DURIN_PROJECT_SOURCE_DIR}/Runtime/VulkanRHI/Private
	COMPILE_DEFINITIONS DURIN_VULKAN_TEST_FAILURE_INJECTION=1
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	RESOURCE_LOCKS durin-gpu durin-rhi-lifecycle
	HEAVY_RUNTIME_RATIONALE
		"Exercises view-local StaticMesh material preparation against initialized render resources."
)

durin_add_native_test(DirectionalShadowBaselineVulkanTests
	KIND qualification
	DOMAINS renderer shadow
	MODULES asset-forge-builtins engine renderer
	BACKENDS vulkan
	STACKS editor renderer
	TIMEOUT 900
	RUNTIME_ONLY_RATIONALE "RHIInit selects VulkanRHI dynamically for the hardware-backed baseline captures."
	RUNTIME_ONLY_TARGETS VulkanRHI
	SOURCES Private/DirectionalShadowBaselineVulkanTests.cpp
	LIBRARIES Core CoreDObject Engine AssetForgeBuiltins ApplicationCore RenderCore Renderer
	DATA_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Data/DirectionalShadowQ0
		${CMAKE_CURRENT_SOURCE_DIR}/Data/DirectionalShadowQ1
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	RESOURCE_LOCKS durin-gpu durin-rhi-lifecycle
	HEAVY_RUNTIME_RATIONALE
		"Captures the frozen Q0 directional-shadow Lit baseline through the production Vulkan renderer."
)

durin_add_native_test(HDRDisplayMappingQualificationTests
	KIND qualification
	DOMAINS renderer viewport
	MODULES engine renderer vulkan-rhi
	BACKENDS vulkan
	STACKS renderer
	TIMEOUT 900
	SOURCES Private/HDRDisplayMappingQualificationTests.cpp
	LIBRARIES Core CoreDObject Engine ApplicationCore RenderCore Renderer VulkanRHI Vulkan::Vulkan
	INCLUDE_DIRECTORIES
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		${DURIN_PROJECT_SOURCE_DIR}/Runtime/VulkanRHI/Private
	DATA_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/Data/HDRDisplayMapping
	ENVIRONMENTS authored-shaders
	RESOURCE_LOCKS durin-gpu durin-rhi-lifecycle
	HEAVY_RUNTIME_RATIONALE
		"Measures HDR copy and FXAA display routes and applies the frozen 1920x1080 RTX 3090 gate only when the selected Vulkan adapter matches it."
)

durin_add_native_test(GBufferQualificationTests
	KIND qualification
	DOMAINS renderer
	MODULES asset-forge-builtins engine renderer vulkan-rhi
	BACKENDS vulkan
	STACKS editor renderer
	TIMEOUT 900
	SOURCES Private/GBufferQualificationTests.cpp
	LIBRARIES
		Core
		CoreDObject
		Engine
		AssetForgeBuiltins
		ApplicationCore
		RenderCore
		Renderer
		VulkanRHI
		Vulkan::Vulkan
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		${DURIN_PROJECT_SOURCE_DIR}/Runtime/VulkanRHI/Private
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	RESOURCE_LOCKS durin-gpu durin-rhi-lifecycle
	HEAVY_RUNTIME_RATIONALE
		"Measures the four-family GBuffer path and applies the frozen 1920x1080 RTX 3090 gate only when the selected Vulkan adapter matches it."
)

durin_add_native_test(VolumetricCloudQualificationTests
	KIND qualification
	DOMAINS renderer
	MODULES engine renderer vulkan-rhi
	BACKENDS vulkan
	STACKS renderer
	TIMEOUT 900
	SOURCES Private/VolumetricCloudQualificationTests.cpp
	LIBRARIES Core CoreDObject Engine ApplicationCore RenderCore Renderer VulkanRHI Vulkan::Vulkan
	INCLUDE_DIRECTORIES
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		${DURIN_PROJECT_SOURCE_DIR}/Runtime/VulkanRHI/Private
	ENVIRONMENTS authored-shaders
	RESOURCE_LOCKS durin-gpu durin-rhi-lifecycle
	HEAVY_RUNTIME_RATIONALE
		"Measures the frozen volumetric-cloud compute and fragment routes across the P1 extent matrix."
)

durin_add_native_test(EditorRenderingTests
	KIND feature
	DOMAINS renderer
	MODULES asset-tools durin-ed engine renderer static-mesh-build texture-build asset-forge-builtins
	STACKS editor renderer
	SOURCES
		Private/EditorGridRenderingTests.cpp
		Private/PrimitiveDrawInterfaceTests.cpp
		Private/SimpleElementCollectorTests.cpp
		Private/RendererEditorAssistanceTests.cpp
		Private/RendererResourceInvalidationTests.cpp
		Private/RendererResourceSlotCacheTests.cpp
		Private/RendererRenderTargetLayoutTests.cpp
		Private/RendererSceneViewTests.cpp
		Private/EditorTextureSmokeTests.cpp
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${_durin_level_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
		${_durin_material_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MaterialEditor/Public
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Engine/Private
	LIBRARIES
		Core
		CoreDObject
		Engine
		ApplicationCore
		AssetTools
		AssetForgeBuiltins
		RenderCore
		Renderer
		DurinEd
		MaterialEditor
		StaticMeshBuild
		TextureBuild
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport ${CMAKE_CURRENT_SOURCE_DIR}/Data
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	HEAVY_RUNTIME_RATIONALE "Exercises renderer-backed editor assistance and grid rendering."
)

if(NOT APPLE OR DURIN_ENABLE_APPLICATION_TESTS)
	durin_add_native_test(EditorGridVulkanTests
		EXECUTION_HOST application
		KIND ${_durin_vulkan_integration_kind}
		DOMAINS renderer viewport
		MODULES engine renderer vulkan-rhi
		BACKENDS vulkan
		STACKS editor renderer
		TIMEOUT 900
		SOURCES Private/EditorGridVulkanTests.cpp
		LIBRARIES Core CoreDObject Engine ApplicationCore RenderCore Renderer VulkanRHI Vulkan::Vulkan
		INCLUDE_DIRECTORIES
			${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
			${DURIN_PROJECT_SOURCE_DIR}/Runtime/VulkanRHI/Private
		COMPILE_DEFINITIONS DURIN_VULKAN_TEST_FAILURE_INJECTION=1
		REQUIRES editor
		REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
		ENVIRONMENTS authored-shaders
		RESOURCE_LOCKS durin-gpu durin-rhi-lifecycle
		HEAVY_RUNTIME_RATIONALE
			"Exercises the production editor-grid shader and assistance pass through the Vulkan renderer."
	)

	durin_add_native_test(VolumetricCloudVulkanTests
		EXECUTION_HOST application
		KIND ${_durin_vulkan_integration_kind}
		DOMAINS renderer
		MODULES engine renderer vulkan-rhi
		BACKENDS vulkan
		STACKS renderer
		TIMEOUT 900
		SOURCES Private/VolumetricCloudVulkanTests.cpp
		LIBRARIES Core CoreDObject Engine ApplicationCore RenderCore Renderer VulkanRHI Vulkan::Vulkan
		INCLUDE_DIRECTORIES
			${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
			${DURIN_PROJECT_SOURCE_DIR}/Runtime/VulkanRHI/Private
		COMPILE_DEFINITIONS DURIN_VULKAN_TEST_FAILURE_INJECTION=1
		REQUIRES editor
		REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
		ENVIRONMENTS authored-shaders
		RESOURCE_LOCKS durin-gpu durin-rhi-lifecycle
		HEAVY_RUNTIME_RATIONALE
			"Owns one isolated Vulkan lifecycle for compute and fragment volumetric-cloud parity."
	)

	durin_add_native_test(VolumetricCloudSceneVulkanTests
		EXECUTION_HOST application
		KIND ${_durin_vulkan_integration_kind}
		DOMAINS renderer viewport
		MODULES engine renderer vulkan-rhi
		BACKENDS vulkan
		STACKS renderer
		TIMEOUT 900
		SOURCES Private/VolumetricCloudSceneVulkanTests.cpp
		LIBRARIES Core CoreDObject Engine ApplicationCore RenderCore Renderer VulkanRHI Vulkan::Vulkan
		INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		REQUIRES editor
		REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
		ENVIRONMENTS authored-shaders
		RESOURCE_LOCKS durin-gpu durin-rhi-lifecycle
		HEAVY_RUNTIME_RATIONALE
			"Exercises enabled volumetric clouds through SceneRenderer offscreen and window-backed Present routes."
	)
else()
	durin_exclude_native_test_sources(
		RATIONALE
			"Window-backed editor-grid Vulkan qualification runs only when application tests are explicitly enabled."
		SOURCES Private/EditorGridVulkanTests.cpp
			Private/VolumetricCloudVulkanTests.cpp
			Private/VolumetricCloudSceneVulkanTests.cpp
	)
endif()

durin_add_native_test(AssetPackageReloadVulkanTests
	KIND qualification
	DOMAINS asset-package renderer
	MODULES durin-ed engine renderer texture-build
	BACKENDS vulkan
	STACKS editor renderer
	RUNTIME_ONLY_RATIONALE "RHIInit selects VulkanRHI dynamically for this offscreen test."
	RUNTIME_ONLY_TARGETS VulkanRHI
	SOURCES Private/AssetPackageReloadVulkanTests.cpp
	LIBRARIES Core CoreDObject Engine DurinEd TextureBuild RenderCore Renderer
	INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	RESOURCE_LOCKS durin-gpu durin-rhi-lifecycle
	HEAVY_RUNTIME_RATIONALE "Qualifies saved package discard with production cloud rendering and pixel readback."
)

durin_add_native_test(AssetPackageReloadTests
	KIND feature
	DOMAINS asset-package editor-shell
	MODULES durin-ed engine texture-build shader-build
	STACKS editor
	SOURCES Private/AssetPackageReloadTests.cpp
	LIBRARIES Core CoreDObject Engine DurinEd TextureBuild ShaderBuild
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	HEAVY_RUNTIME_RATIONALE
		"Exercises texture/material/function package replacement, editor discard, live references and accepted compiled caller generations."
)

durin_add_native_test(EditorShellTests
	KIND feature
	DOMAINS editor-shell
	MODULES durin-ed level-editor
	STACKS editor
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned shell model white-box coverage avoids exporting private workspace and panel implementations."
	SOURCES
		Private/EditorBootstrapStateTests.cpp
		Private/EditorNotificationTests.cpp
		Private/EditorWorkspaceTests.cpp
		Private/UIStyleTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Workspace/LevelEditorContext.cpp
		${_durin_level_editor_private}/Viewport/ViewportPickingSceneIndex.cpp
	LIBRARIES Core CoreDObject Engine ApplicationCore MonaCore Mona MonaImGui DurinEd
	INCLUDE_DIRECTORIES
		${_durin_level_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
		${_durin_content_browser_public}
		${CMAKE_SOURCE_DIR}/Engine/Source
	COMPILE_DEFINITIONS LEVELEDITOR_EXPORTS
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	HEAVY_RUNTIME_RATIONALE "Exercises DurinEd and Mona editor-shell models."
)

durin_add_native_test(EditorHostToolTests
	KIND feature
	DOMAINS editor-shell
	MODULES durin-ed main-frame
	STACKS editor
	PRIVATE_SOURCE_OWNER MainFrame
	PRIVATE_SOURCE_RATIONALE
		"MainFrame-owned Console model and layout coverage avoids exporting private host-tool implementations."
	SOURCES Private/ConsoleRecordModelTests.cpp Private/EditorHostToolTests.cpp
	PRIVATE_SOURCES ${_durin_main_frame_private}/Panels/ConsoleRecordModel.cpp
	LIBRARIES Core CoreDObject Engine ApplicationCore MonaCore Mona MonaImGui DurinEd
	INCLUDE_DIRECTORIES ${_durin_main_frame_private}
	COMPILE_DEFINITIONS MAINFRAME_EXPORTS
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	HEAVY_RUNTIME_RATIONALE "Exercises MainFrame host-tool models."
)

durin_add_native_test(ExternalToolTests
	KIND feature
	DOMAINS editor-shell
	MODULES durin-ed main-frame
	STACKS editor
	PRIVATE_SOURCE_OWNER MainFrame
	PRIVATE_SOURCE_RATIONALE
		"MainFrame-owned profiling integration white-box coverage avoids exporting the private service implementation."
	TIMEOUT 600
	SOURCES Private/ProfilingToolServiceTests.cpp
	PRIVATE_SOURCES ${_durin_main_frame_private}/ProfilingToolService.cpp
	LIBRARIES Core CoreDObject Engine ApplicationCore MonaCore Mona MonaImGui DurinEd
	INCLUDE_DIRECTORIES ${_durin_main_frame_private}
	COMPILE_DEFINITIONS MAINFRAME_EXPORTS
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	HEAVY_RUNTIME_RATIONALE "Exercises the DurinEd profiling-tool integration."
)

# Cooked-runtime mode and Renderer/Vulkan teardown are process-global.
durin_add_native_test(TextureCookIntegrationTests
	KIND ${_durin_vulkan_integration_kind}
	DOMAINS asset-cook texture
	MODULES asset-tools engine static-mesh-build renderer asset-forge-builtins vulkan-rhi
	BACKENDS vulkan
	STACKS editor renderer
	TIMEOUT 900
	SOURCES Private/Texture/TextureCookTests.cpp
	LIBRARIES
		Core
		CoreDObject
		Engine
		AssetTools
		StaticMeshBuild
		TextureBuild
		AssetForgeBuiltins
		RenderCore
		Renderer
		VulkanRHI
		Vulkan::Vulkan
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		${DURIN_PROJECT_SOURCE_DIR}/Runtime/VulkanRHI/Private
	COMPILE_DEFINITIONS DURIN_VULKAN_TEST_FAILURE_INJECTION=1
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	RESOURCE_LOCKS durin-gpu durin-rhi-lifecycle
	HEAVY_RUNTIME_RATIONALE "Owns the renderer and Vulkan cooked-texture lifecycle."
)

# Measures material first use separately from routine renderer correctness coverage.
set(_durin_material_qualification_platform_libraries)
if(WIN32)
	set(_durin_material_qualification_platform_libraries dxgi)
endif()

durin_add_native_test(MaterialCreationQualificationTests
	KIND qualification
	DOMAINS material renderer rhi-creation
	MODULES asset-forge-builtins engine renderer vulkan-rhi
	BACKENDS vulkan
	STACKS renderer
	TIMEOUT 900
	SOURCES Private/MaterialCreationQualificationTests.cpp
	LIBRARIES
		${_durin_material_qualification_platform_libraries}
		Core
		CoreDObject
		Engine
		AssetForgeBuiltins
		ApplicationCore
		RenderCore
		Renderer
		VulkanRHI
		Vulkan::Vulkan
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		${DURIN_PROJECT_SOURCE_DIR}/Runtime/VulkanRHI/Private
		${CMAKE_SOURCE_DIR}/Engine/Tests/Native/VulkanRHITests/Private
	COMPILE_DEFINITIONS DURIN_VULKAN_TEST_FAILURE_INJECTION=1
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	RESOURCE_LOCKS durin-gpu durin-rhi-lifecycle
	HEAVY_RUNTIME_RATIONALE "Measures a fixed material scene through complete production renderer frames."
)
