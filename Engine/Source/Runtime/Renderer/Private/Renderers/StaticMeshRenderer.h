#pragma once

#include "CoreMinimal.h"

#include "Renderers/SceneRendererProfiling.h"

#include "Misc/CoreTypes.h"
#include "RendererAPI.h"
#include "Renderers/MeshRenderPreparationCommon.h"

namespace Durin
{
	class FStaticMeshSectionResourceContext;
	enum class EStaticMeshSectionResourcePolicy : uint8;
	class FGBufferRenderer;
	class FScene;
	class FRendererResourceCoordinator;
	namespace RendererPrivate { class FSurfaceMaterialResources; }
	class FRHICommandListImmediate;
	class FRHICommandList;
	struct FStaticMeshRenderObservations;
	struct FPreparedStaticMeshView;
	struct FResolvedStaticMeshView;
	namespace RendererPrivate { struct FMeshPassPipeline; }
	struct FResolvedMeshDrawRecord;
	struct FPreparedStaticMeshDraw;
	struct FPreparedStaticMeshPrimitive;
	struct FMaterialRenderBinding;
	enum class ERasterMode : uint8;
	enum class ERenderMode : uint8;
	struct FRHIUniformBufferRange;
	struct FSceneView;
	enum class EMeshBasePass : uint8;

	// Owns StaticMesh shaders, material pipelines, and proxy draw submission.
	class FStaticMeshRenderer final
	{
	public:
		auto PrepareGBufferPipelines_RenderThread(FGBufferRenderer& GBuffer, const FPreparedStaticMeshView& PreparedView,
			FResolvedStaticMeshView& ResolvedView,
			bool bPrepareGPUCulling = false, EGPUCullingFailure Failure = EGPUCullingFailure::None) -> bool;
			auto PrepareBindings_RenderThread(FRHICommandListImmediate& CommandList, FGBufferRenderer* GBuffer,
			const FPreparedStaticMeshView& PreparedView, FResolvedStaticMeshView& ResolvedView,
			const FRHIUniformBufferRange& Lighting, bool bShadow = false) -> bool;
		FStaticMeshRenderer(
			FRendererResourceCoordinator& InCoordinator,
			RendererPrivate::FSurfaceMaterialResources& InSurfaceMaterials
		);
		~FStaticMeshRenderer();
		// Pure recording over prepared immutable resources; counters belong to the caller.
		RENDERER_API static auto RecordShadow(FRHICommandList& CommandList,
			const FPreparedStaticMeshView& PreparedView, const FResolvedStaticMeshView& ResolvedView)
			-> FStaticMeshRenderObservations;

		FStaticMeshRenderer(const FStaticMeshRenderer&) = delete;
		auto operator=(const FStaticMeshRenderer&)
			-> FStaticMeshRenderer& = delete;

		auto PrepareResources_RenderThread(FRHICommandListImmediate& CommandList,
			const FPreparedStaticMeshView& PreparedView, FResolvedStaticMeshView& ResolvedView,
			bool bPrepareLitOpaqueForward, FStaticMeshSectionResourceContext* ResourceContext = nullptr)
			-> FGeometryResolutionResult;
		auto PrepareHybridRetainedResources_RenderThread(
			const FPreparedStaticMeshView& PreparedView,
			FResolvedStaticMeshView& ResolvedView
		) -> bool;
		auto PrepareShadowResources_RenderThread(FRHICommandListImmediate& CommandList,
			const FPreparedStaticMeshView& PreparedView, FResolvedStaticMeshView& ResolvedView,
			FStaticMeshSectionResourceContext* ResourceContext = nullptr) -> FGeometryResolutionResult;
		auto Execute_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FRHIUniformBufferRange& Lighting,
			ERenderMode RenderMode,
			const FPreparedStaticMeshView& PreparedView,
			FResolvedStaticMeshView& ResolvedView
		) -> void;
		auto ExecutePass_RenderThread(
			FRHICommandListImmediate& CommandList, const FSceneView& View, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, EMeshBasePass Pass, const FPreparedStaticMeshView& PreparedView, FResolvedStaticMeshView& ResolvedView
		) -> void;
		auto ExecutePreparedDraw_RenderThread(
			FRHICommandListImmediate& CommandList, const FSceneView& View, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, EMeshBasePass Pass, const FPreparedStaticMeshDraw& Draw, const FPreparedStaticMeshView& PreparedView, FResolvedStaticMeshView& ResolvedView, bool bHybridRetained = false, FMeshDrawCommandStateCache* BindingGroup = nullptr
		) -> void;
		auto FinalizeExecution_RenderThread(FResolvedStaticMeshView& ResolvedView)
			-> void;
		auto ExecuteGBuffer_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			FGBufferRenderer& GBuffer,
			const FPreparedStaticMeshView& PreparedView,
			FResolvedStaticMeshView& ResolvedView,
			const std::vector<uint32>* IndirectArgumentByResolvedDraw = nullptr,
			FRHIBuffer* IndirectArguments = nullptr,
			FRHIBuffer* VisibleInstances = nullptr,
			FRHIBuffer* InstanceTransforms = nullptr
		) -> FGeometryExecutionResult;
		auto ReleaseResources_RenderThread() -> void;

		auto PrepareUniforms_RenderThread(FRHICommandListImmediate& CommandList,
			FScene* Scene, const FSceneView& View, const FPreparedStaticMeshView& PreparedView,
			FResolvedStaticMeshView& ResolvedView, bool bProductionDeferred,
			bool bGBuffer, bool bShadow = false, bool bAllowPartial = false) -> bool;

	private:
		auto PrepareSectionResources_RenderThread(const FPreparedStaticMeshPrimitive* Primitive,
			const FPreparedStaticMeshDraw& Draw, FResolvedMeshDrawRecord& Record,
			EStaticMeshSectionResourcePolicy Policy, FStaticMeshSectionResourceContext* ResourceContext) -> bool;
		static auto RecordPreparedSection(
			FRHICommandList& CommandList,
			const FPreparedStaticMeshPrimitive& Primitive,
			const FPreparedStaticMeshDraw& Item,
			const FResolvedStaticMeshView& ResolvedView,
			bool bShadowDepth = false,
			bool bHybridRetained = false,
			FMeshDrawCommandStateCache* BindingGroup = nullptr
		) -> bool;
		auto EnsureMaterialSamplers_RenderThread(
			const FMaterialRenderBinding& MaterialBinding
		) -> bool;
		auto EnsureSectionResources_RenderThread(
			const FPreparedStaticMeshPrimitive& Primitive,
			const FPreparedStaticMeshDraw& Item,
			const FMaterialRenderBinding& MaterialBinding,
			std::shared_ptr<const RendererPrivate::FMeshPassPipeline>& OutPipeline,
			bool bShadowDepth = false,
			bool bHybridRetained = false
		) -> bool;
		auto DrawGBufferSection_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			FGBufferRenderer& GBuffer,
			const FPreparedStaticMeshPrimitive& Primitive,
			const FPreparedStaticMeshDraw& Item,
			const FResolvedStaticMeshView& ResolvedView,
			FMeshDrawCommandStateCache& BindingGroup,
			FRHIBuffer* IndirectArguments = nullptr,
			uint64 IndirectArgumentOffset = 0,
			FRHIBuffer* VisibleInstances = nullptr,
			FRHIBuffer* InstanceTransforms = nullptr
		) -> bool;
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		RendererPrivate::FSurfaceMaterialResources& SurfaceMaterials;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
