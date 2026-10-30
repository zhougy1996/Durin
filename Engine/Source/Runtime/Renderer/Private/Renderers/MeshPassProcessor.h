#pragma once

#include "CoreMinimal.h"
#include "Materials/PSOPrecacheMaterial.h"
#include "Renderers/MeshPipelineDescription.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "RenderResourceCreation.h"
#include "RendererAPI.h"
#include "Shader/MaterialShader.h"

namespace Durin
{
	class FShaderType;
	class FMeshMaterialShaderMap;
}

namespace Durin::RendererPrivate
{
	class FMeshVertexFactoryImplementation;
	class FMeshVertexShaderBinding;
	class FCompiledSurfaceBindingLayout;
	struct FMeshPassFragmentShader
	{
		FShaderType* Type = nullptr;
		FRHIShader* (*Resolve)(const FMeshMaterialShaderMap&) = nullptr;
		const char* FixedModule = nullptr;
		const char* FixedRequest = nullptr;
		const FCompiledSurfaceBindingLayout* (*ResolveLayout)(const FMeshMaterialShaderMap&) = nullptr;
	};
	// A borrowed projection of an already admitted batch element. Visibility and
	// geometry ownership remain with the existing prepared mesh representation.
	struct FMeshPassProcessorBatch
	{
		const FMaterialPlanningPassIdentity& Material;
		const FMeshVertexFactoryImplementation& Factory;
		FMeshPipelineState PipelineState;
		FGraphicsPipelineStateInitializer::EPrimitiveTopology Topology;
		FPrimitiveRenderParticipation Participation;
		bool bSupportsGBuffer = true;
		bool bGPUCulling = false;
		bool bReversedZ = false;
	};
	struct FMeshPassShaders
	{
		FMeshMaterialShaderMap ShaderMap;
		std::shared_ptr<const FMeshVertexShaderBinding> VertexShader;
		FShaderRHIRef FragmentShader;
		const FCompiledSurfaceBindingLayout* FragmentLayout = nullptr;
		uint32 VertexFactoryPass = 0;
		const FShaderType* FragmentType = nullptr;
		FXxHash64 FactoryKey;
		FXxHash64 LayoutKey;
	};
	struct FMeshPassPipeline : FMeshPassShaders
	{
		FVertexDeclarationRHIRef VertexDeclaration;
		FGraphicsPipelineStateRHIRef PipelineState;
	};
	struct FMeshDrawPrimitiveState
	{
		FMatrix LocalToWorld;
		FMatrix WorldToLocal;
		FMatrix4f NormalToWorld;
		FVector3 BoundsCenter;
		FPrimitiveCustomData CustomPrimitiveData;
		FPrimitiveCustomData CustomInstanceData;
		auto operator==(const FMeshDrawPrimitiveState&) const -> bool = default;
		static auto Capture(const FPreparedStaticMeshPrimitive& Primitive) -> FMeshDrawPrimitiveState
		{
			return {Primitive.LocalToWorld, Primitive.WorldToLocal, Primitive.NormalToWorld, Primitive.BoundsCenter,
				Primitive.CustomPrimitiveData, Primitive.CustomInstanceData};
		}
	};
	// Complete retained draw snapshot. Per-view bindings are refreshed separately.
	struct FMeshDrawCommand
	{
		using FKey = std::array<uint64, 7>;
		static auto MakeKey(const FMeshDrawDescription* Description, FPrimitiveComponentId PrimitiveId,
			uint64 BatchId, const FResolvedMeshDrawRecord& Record) -> FKey
		{
			return {reinterpret_cast<uint64>(Description), PrimitiveId.Value, BatchId,
				reinterpret_cast<uint64>(Record.Pipeline.get()), reinterpret_cast<uint64>(Record.HybridPipeline.get()),
				reinterpret_cast<uint64>(Record.GBufferPipeline.get()), reinterpret_cast<uint64>(Record.GBufferGPUCullingPipeline.get())};
		}
		auto GetKey() const -> FKey { return MakeKey(Description.get(), PrimitiveId, BatchId, Record); }
		FPrimitiveComponentId PrimitiveId = InvalidPrimitiveComponentId;
		uint64 BatchId = 0;
		std::shared_ptr<const FMeshDrawDescription> Description;
		std::shared_ptr<const FVertexFactoryInputBinding> Input;
		FRHIUniformBufferRange PrimitiveUniform;
		FResolvedMeshDrawRecord Record;
		FMeshDrawPrimitiveState PrimitiveState;
		std::array<std::optional<FResolvedStaticMeshView::FMaterialUniform>, 3> Materials;
	};
	// ShaderMap and pipeline caches keep their existing owners. Each cache miss
	// supplies one of these contexts; precaching emits initializers without PSOs.
	class FMeshPassDrawListContext
	{
	public:
		RENDERER_API FMeshPassDrawListContext(const FRenderResourceGeneration& Generation,
			const FMaterialCompilerResult* Program, const FShaderCompileOptions& Options, FMeshPassShaders& OutShaders);
		RENDERER_API FMeshPassDrawListContext(FName Name, const FMeshPassShaders& Shaders,
			FRHIVertexDeclaration* Declaration, FMeshPassPipeline& OutCommand);
		RENDERER_API FMeshPassDrawListContext(const FMeshPassShaders& Shaders,
			FRHIVertexDeclaration* Declaration, FPSOPrecacheDataArray& OutInitializers, bool bRequired, EPSOPrecachePriority Priority);
		RENDERER_API FMeshPassDrawListContext(const FPreparedStaticMeshPrimitive& Primitive,
			const FPreparedStaticMeshDraw& Draw, const FResolvedStaticMeshView& ResolvedView,
			std::shared_ptr<const FMeshDrawCommand>& OutCommand);
		auto GetResult() const -> const FShaderCompletion& { return Result; }
		auto GetFailureCategory() const -> ERenderResourceCreateErrorCategory { return FailureCategory; }
	private:
		friend class FMeshPassProcessor;
		FRenderResourceGeneration Generation;
		const FMaterialCompilerResult* Program = nullptr;
		FShaderCompileOptions Options;
		FMeshPassShaders* OutShaders = nullptr;
		const FMeshPassShaders* Shaders = nullptr;
		FRHIVertexDeclaration* Declaration = nullptr;
		FMeshPassPipeline* OutCommand = nullptr;
		FPSOPrecacheDataArray* OutInitializers = nullptr;
		const FPreparedStaticMeshPrimitive* Primitive = nullptr;
		const FPreparedStaticMeshDraw* Draw = nullptr;
		const FResolvedStaticMeshView* ResolvedView = nullptr;
		std::shared_ptr<const FMeshDrawCommand>* OutRetainedCommand = nullptr;
		FName Name;
		bool bRequired = false;
		EPSOPrecachePriority Priority = EPSOPrecachePriority::Medium;
		FShaderCompletion Result = FShaderCompletion::Canceled();
		ERenderResourceCreateErrorCategory FailureCategory = ERenderResourceCreateErrorCategory::InvalidConfiguration;
	};
	struct FMeshPSOPrecacheVariant
	{
		FMeshPipelineRasterOptions Raster;
		bool bRequired = false;
	};
	struct FMeshPSOCollectorContext
	{
		FRenderResourceGeneration Generation;
		EPSOPrecachePriority Priority = EPSOPrecachePriority::Medium;
	};

	// One pass owns eligibility, shader selection and pipeline description for both
	// drawing and collection. Geometry visibility and native submission stay outside.
	class FMeshPassProcessor : public IPSOCollector
	{
	public:
		RENDERER_API explicit FMeshPassProcessor(FMeshPSOCollectorContext Context = {});
		RENDERER_API ~FMeshPassProcessor() override;
		virtual auto GetPass() const -> EMeshPipelinePass = 0;
		virtual auto AddMeshBatch(const FMeshPassProcessorBatch& MeshBatch, FMeshPassDrawListContext& Context) const -> void = 0;
		virtual auto IsEnabled(const FSceneRenderPreparationConfig& Config) const -> bool = 0;
		RENDERER_API auto IsMeshRelevant(const FMaterialPlanningPassIdentity& Material,
			const FPrimitiveRenderParticipation& Participation = {}, bool bSupportsGBuffer = true) const -> bool;
		RENDERER_API auto SupportsVertexFactory(const FMeshVertexFactoryImplementation& Factory) const -> bool;
		virtual auto GetVertexFactoryPass(bool bGPUCulling = false) const -> uint32 = 0;
		virtual auto GetFragmentShader(FMaterialShaderBlendModeKey BlendMode) const -> FMeshPassFragmentShader = 0;
		virtual auto DescribeMeshDraw(const FMeshPipelineState& State,
			FGraphicsPipelineStateInitializer::EPrimitiveTopology Topology,
			FMaterialShaderBlendModeKey BlendMode = EMaterialBlendMode::Opaque, bool bReversedZ = false) const -> FMeshPipelineDescription = 0;
		virtual auto GetPSOPrecacheVariants(const FSceneRenderPreparationConfig& Config,
			const FPSOPrecacheParams& Params) const -> std::vector<FMeshPSOPrecacheVariant> = 0;

		RENDERER_API auto CollectPSOInitializers(const FSceneRenderPreparationConfig& SceneConfig,
			const FMaterialResource& Material, const FPSOPrecacheVertexFactoryData& VertexFactoryData,
			const FPSOPrecacheParams& PrecacheParams, FPSOPrecacheDataArray& PSOInitializers) -> void override;
		// Async adaptation around the UE-shaped append interface. Required collection
		// finishes across all passes before any optional work is allowed to fail.
		auto SetCollectRequiredPSOs(bool bRequired) -> void { CollectionPhase = bRequired; }
		RENDERER_API auto GetPSOCollectionResult() const -> std::expected<bool, std::string>;
	protected:
		RENDERER_API auto BuildMeshDrawCommands(const FMeshPassProcessorBatch& MeshBatch, FMeshPassDrawListContext& Context) const -> void;
		RENDERER_API static auto RasterVariants(const FSceneRenderPreparationConfig& Config,
			const FPSOPrecacheParams& Params, uint8 Count, bool bShadow = false, bool bPicking = false)
			-> std::vector<FMeshPSOPrecacheVariant>;
	private:
		struct FState;
		std::unique_ptr<FState> State;
		FMeshPSOCollectorContext Context;
		// Unset is the ordinary UE contract: collect every applicable initializer.
		std::optional<bool> CollectionPhase;
		auto CollectPhase(const FSceneRenderPreparationConfig& SceneConfig,
			const FMaterialResource& Material, const FPSOPrecacheVertexFactoryData& VertexFactoryData,
			const FPSOPrecacheParams& PrecacheParams, FPSOPrecacheDataArray& PSOInitializers, bool bRequired) -> void;
	};

	using PassProcessorCreateFunction = std::unique_ptr<FMeshPassProcessor> (*)(FMeshPSOCollectorContext);

	// Registration is module-initialization owned; removal requires quiescent users.
	// No concrete pass implementation or creation switch belongs to this manager.
	class FPassProcessorManager
	{
	public:
		RENDERER_API static auto IsRegistered(EMeshPipelinePass Pass) -> bool;
		RENDERER_API static auto GetRegisteredPasses() -> std::vector<EMeshPipelinePass>;
		RENDERER_API static auto GetMeshPassProcessor(EMeshPipelinePass Pass) -> const FMeshPassProcessor&;
		// Each collection request owns a fresh instance and its asynchronous state.
		RENDERER_API static auto CreateMeshPassProcessor(EMeshPipelinePass Pass, FMeshPSOCollectorContext Context = {})
			-> std::unique_ptr<FMeshPassProcessor>;
	};

	class FRegisterPassProcessorCreateFunction
	{
	public:
		RENDERER_API FRegisterPassProcessorCreateFunction(PassProcessorCreateFunction CreateFunction, EMeshPipelinePass Pass);
		RENDERER_API ~FRegisterPassProcessorCreateFunction();
		FRegisterPassProcessorCreateFunction(const FRegisterPassProcessorCreateFunction&) = delete;
		auto operator=(const FRegisterPassProcessorCreateFunction&) -> FRegisterPassProcessorCreateFunction& = delete;
		auto IsRegistered() const -> bool { return bRegistered; }
	private:
		PassProcessorCreateFunction CreateFunction;
		EMeshPipelinePass Pass;
		bool bRegistered = false;
	};
}
