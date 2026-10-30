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
		EMeshPipelinePass Pass = EMeshPipelinePass::Forward;
		bool bGPUCulling = false;
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
	// One immutable command for one pass. Cached commands contain only stable
	// bindings; dynamic commands additionally own the view/pass binding packets.
	struct FMeshDrawCommand
	{
		using FKey = std::array<uint64, 6>;
		static auto MakeKey(const FMeshDrawDescription* Description, FPrimitiveComponentId PrimitiveId,
			uint64 BatchId, const FMeshPassPipeline& Pipeline) -> FKey
		{
			return {reinterpret_cast<uint64>(Description), PrimitiveId.Value, BatchId,
				static_cast<uint64>(Pipeline.Pass), Pipeline.bGPUCulling, reinterpret_cast<uint64>(&Pipeline)};
		}
		auto GetKey() const -> FKey { return MakeKey(Description.get(), PrimitiveId, BatchId, *Pipeline); }
		RENDERER_API auto SubmitBindings(FRHICommandList& Commands) const -> bool;
		FPrimitiveComponentId PrimitiveId = InvalidPrimitiveComponentId;
		uint64 BatchId = 0;
		std::shared_ptr<const FMeshDrawDescription> Description;
		std::shared_ptr<const FVertexFactoryInputBinding> Input;
		std::shared_ptr<const FMeshPassPipeline> Pipeline;
		FRHIUniformBufferRange PrimitiveUniform;
		FMeshDrawPrimitiveState PrimitiveState;
		FResolvedStaticMeshView::FMaterialUniform Material;
		std::shared_ptr<const FRHIShaderParameterBatch> VertexBindings;
		std::shared_ptr<const FPreparedSurfaceMaterialBindings> FragmentBindings;
		std::shared_ptr<const FPreparedSurfaceMaterialBindings> VertexPassBindings;
		std::shared_ptr<const FPreparedSurfaceMaterialBindings> FragmentPassBindings;
		FRHIUniformBufferRange ViewUniform;
		bool bCached = false;
	};
	struct FMeshDrawCommandBuildInput
	{
		FRHICommandListImmediate& Commands;
		const FPreparedStaticMeshPrimitive& Primitive;
		const FPreparedStaticMeshDraw& Draw;
		std::shared_ptr<const FMeshPassPipeline> Pipeline;
		const FResolvedStaticMeshView::FMaterialUniform& Material;
		FRHIUniformBufferRange PrimitiveUniform;
		FRHIUniformBufferRange ViewUniform;
		FRHIUniformBufferRange Lighting;
		FRHIUniformBufferRange HitProxy;
		uint32 MaterialSlot = 0;
	};
	// One view-preparation scope shares existing material/primitive packets.
	struct FMeshDrawCommandBindingCache
	{
		using FBindings = std::shared_ptr<const FPreparedSurfaceMaterialBindings>;
		std::map<std::tuple<uint32, uint32, uint32, FRHIShader*>, FBindings> Fragments;
		std::map<std::pair<uint32, FRHIShader*>, FBindings> Passes;
		std::map<std::tuple<uint32, uint32, uint32, FRHIShader*>, std::shared_ptr<const FRHIShaderParameterBatch>> Vertices;
		size_t PreparedSurfaceBindingBatches = 0, SurfaceBindingBatchReuses = 0;
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
		auto GetResult() const -> const FShaderCompletion& { return Result; }
		auto GetFailureCategory() const -> ERenderResourceCreateErrorCategory { return FailureCategory; }
	protected:
		RENDERER_API FMeshPassDrawListContext(const FMeshDrawCommandBuildInput& Input, FMeshDrawCommandBindingCache& Cache,
			std::shared_ptr<const FMeshDrawCommand>& Out, bool bCached, std::shared_ptr<const FMeshDrawCommand> Existing);
		RENDERER_API FMeshPassDrawListContext(std::shared_ptr<const FMeshDrawCommand> Prepared,
			FRHIStorageBufferRange VisibleInstances, FRHIStorageBufferRange InstanceTransforms,
			std::shared_ptr<const FMeshDrawCommand>& Out);
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
		const FMeshDrawCommandBuildInput* BindingInput = nullptr;
		FMeshDrawCommandBindingCache* BindingCache = nullptr;
		std::shared_ptr<const FMeshDrawCommand>* OutDrawCommand = nullptr;
		std::shared_ptr<const FMeshDrawCommand> ExistingCommand;
		std::shared_ptr<const FMeshDrawCommand> GPUCommand;
		FRHIStorageBufferRange VisibleInstances, InstanceTransforms;
		bool bCachedOutput = false;
		FName Name;
		bool bRequired = false;
		EPSOPrecachePriority Priority = EPSOPrecachePriority::Medium;
		FShaderCompletion Result = FShaderCompletion::Canceled();
		ERenderResourceCreateErrorCategory FailureCategory = ERenderResourceCreateErrorCategory::InvalidConfiguration;
	};
	class FCachedPassMeshDrawListContext final : public FMeshPassDrawListContext
	{
	public:
		FCachedPassMeshDrawListContext(const FMeshDrawCommandBuildInput& Input, FMeshDrawCommandBindingCache& Cache,
			std::shared_ptr<const FMeshDrawCommand>& Out)
			: FMeshPassDrawListContext(Input, Cache, Out, true, {}) {}
	};
	class FDynamicPassMeshDrawListContext final : public FMeshPassDrawListContext
	{
	public:
		FDynamicPassMeshDrawListContext(const FMeshDrawCommandBuildInput& Input, FMeshDrawCommandBindingCache& Cache,
			std::shared_ptr<const FMeshDrawCommand>& Out, std::shared_ptr<const FMeshDrawCommand> Existing = {})
			: FMeshPassDrawListContext(Input, Cache, Out, false, std::move(Existing)) {}
		FDynamicPassMeshDrawListContext(std::shared_ptr<const FMeshDrawCommand> Prepared,
			FRHIStorageBufferRange VisibleInstances, FRHIStorageBufferRange InstanceTransforms,
			std::shared_ptr<const FMeshDrawCommand>& Out)
			: FMeshPassDrawListContext(std::move(Prepared), VisibleInstances, InstanceTransforms, Out) {}
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
