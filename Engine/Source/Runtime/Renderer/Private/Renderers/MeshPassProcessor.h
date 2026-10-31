#pragma once

#include "CoreMinimal.h"
#include "PSOPrecacheMaterial.h"
#include "Renderers/MeshPipelineDescription.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "RenderResourceCreation.h"
#include "RendererAPI.h"
#include "Shader/MaterialShader.h"
#include "VertexFactory.h"

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
		FMeshPipelineDescription Description;
		EMeshPipelinePass Pass = EMeshPipelinePass::Forward;
		bool bGPUCulling = false;
		auto GetPassKey() const -> FMeshPassKey { return {Pass, bGPUCulling}; }
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
		RENDERER_API auto SetDrawParametersAndFinalize(const FPreparedStaticMeshPrimitive& Primitive,
			const FPreparedStaticMeshDraw& Draw) -> bool;
		RENDERER_API auto SubmitDraw(FRHICommandList& Commands, FMeshDrawCommandStateCache* StateCache = nullptr) const -> bool;
		std::vector<FVertexInputStream> VertexStreams;
		FBufferRHIRef IndexBuffer;
		uint32 IndexBufferOffset = 0;
		std::variant<FRHIDrawArguments, FRHIDrawIndexedArguments> DrawParameters;
		FBufferRHIRef IndirectArgs;
		uint64 IndirectArgsOffset = 0;
		// Constant, slope, clamp, matching the prepared shadow bias projection.
		std::array<float, 3> DepthBias{};
		bool bFinalized = false;
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
	private:
		auto SubmitBindings(FRHICommandList& Commands) const -> bool;
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
	};
	// One view-preparation scope shares existing material/primitive packets.
	struct FMeshDrawCommandBindingCache
	{
		using FBindings = std::shared_ptr<const FPreparedSurfaceMaterialBindings>;
		std::map<std::tuple<uint32, EMeshPipelinePass, uint32, FRHIShader*>, FBindings> Fragments;
		std::map<std::pair<EMeshPipelinePass, FRHIShader*>, FBindings> Passes;
		std::map<std::tuple<uint32, uint32, EMeshPipelinePass, FRHIShader*>, std::shared_ptr<const FRHIShaderParameterBatch>> Vertices;
		size_t PreparedSurfaceBindingBatches = 0, SurfaceBindingBatchReuses = 0;
	};
	// Persistent immutable commands, grouped by pass. Eviction drops only the cache's reference.
	class FCachedMeshDrawCommandStorage
	{
	public:
		FCachedMeshDrawCommandStorage() = default;
		FCachedMeshDrawCommandStorage(const FCachedMeshDrawCommandStorage&) = delete;
		auto operator=(const FCachedMeshDrawCommandStorage&) -> FCachedMeshDrawCommandStorage& = delete;
		RENDERER_API auto Find(FMeshPassKey Pass, const FMeshDrawCommand::FKey& Key) -> std::shared_ptr<const FMeshDrawCommand>;
		RENDERER_API auto Publish(FMeshDrawCommand Command) -> std::shared_ptr<const FMeshDrawCommand>;
		RENDERER_API auto Reset() -> void;
		auto Num() const -> size_t { return Count; }
	private:
		struct FEntry { std::shared_ptr<const FMeshDrawCommand> Command; uint64 LastUsed = 0; };
		std::map<FMeshPassKey, std::map<FMeshDrawCommand::FKey, FEntry>> Passes;
		size_t Count = 0;
		uint64 Serial = 0;
	};
	// Stable-address frame storage. Aliasing handles retain the entire frame allocation
	// through existing resolved-resource owners, including replaced GPU command snapshots.
	class FDynamicMeshDrawCommandStorage
	{
	public:
		RENDERER_API FDynamicMeshDrawCommandStorage();
		FDynamicMeshDrawCommandStorage(const FDynamicMeshDrawCommandStorage&) = delete;
		auto operator=(const FDynamicMeshDrawCommandStorage&) -> FDynamicMeshDrawCommandStorage& = delete;
		RENDERER_API auto Publish(FMeshPassKey Pass, FMeshDrawCommand Command,
			FMeshDrawCommandHandle Replace = {}) -> FMeshDrawCommandHandle;
		RENDERER_API auto GetCommand(FMeshDrawCommandHandle Handle) const -> std::shared_ptr<const FMeshDrawCommand>;
		RENDERER_API auto Num(FMeshPassKey Pass) const -> size_t;
	private:
		struct FData
		{
			uint64 Id = 0;
			std::deque<FMeshDrawCommand> Commands;
			std::map<FMeshPassKey, std::vector<const FMeshDrawCommand*>> Passes;
		};
		std::shared_ptr<FData> Data;
	};
	// Transactional publication; resource preparation and build inputs belong to callers.
	class FMeshPassDrawListContext
	{
	public:
		virtual ~FMeshPassDrawListContext() = default;
		FMeshPassDrawListContext() = default;
		FMeshPassDrawListContext(const FMeshPassDrawListContext&) = delete;
		auto operator=(const FMeshPassDrawListContext&) -> FMeshPassDrawListContext& = delete;
		RENDERER_API virtual auto AddCommand(const FMeshDrawCommand& Initializer) -> FMeshDrawCommand&;
		virtual auto FinalizeCommand() -> void = 0;
		virtual auto IsCached() const -> bool = 0;
	protected:
		RENDERER_API auto TakeCommand(FMeshPassKey Pass) -> FMeshDrawCommand;
	private:
		std::optional<FMeshDrawCommand> Pending;
	};
	class FCachedPassMeshDrawListContext final : public FMeshPassDrawListContext
	{
	public:
		FCachedPassMeshDrawListContext(FCachedMeshDrawCommandStorage& InStorage, FMeshPassKey InPass,
			std::shared_ptr<const FMeshDrawCommand>& Out) : Storage(InStorage), Pass(InPass), Output(Out) {}
		RENDERER_API auto FinalizeCommand() -> void override;
		auto IsCached() const -> bool override { return true; }
	private:
		FCachedMeshDrawCommandStorage& Storage;
		FMeshPassKey Pass;
		std::shared_ptr<const FMeshDrawCommand>& Output;
	};
	class FDynamicPassMeshDrawListContext : public FMeshPassDrawListContext
	{
	public:
		FDynamicPassMeshDrawListContext(FDynamicMeshDrawCommandStorage& InStorage, FMeshPassKey InPass,
			FMeshDrawCommandHandle& Out, FMeshDrawCommandHandle InReplace = {})
			: Storage(InStorage), Pass(InPass), Output(Out), Replace(InReplace) {}
		RENDERER_API auto FinalizeCommand() -> void override;
		auto IsCached() const -> bool override { return false; }
	private:
		FDynamicMeshDrawCommandStorage& Storage;
		FMeshPassKey Pass;
		FMeshDrawCommandHandle& Output;
		FMeshDrawCommandHandle Replace;
	};


	// One pass owns eligibility, shader selection and pipeline description for both
	// drawing and collection. Geometry visibility and native submission stay outside.
	class FMeshPassProcessor : public IPSOCollector
	{
	public:
		RENDERER_API explicit FMeshPassProcessor(int32 InPSOCollectorIndex = -1);
		RENDERER_API ~FMeshPassProcessor() override;
		virtual auto GetPass() const -> EMeshPipelinePass = 0;
		virtual auto AddMeshBatch(const FMeshPassProcessorBatch& MeshBatch, const FMeshDrawCommandBuildInput& Input,
			FMeshDrawCommandBindingCache& Cache, FMeshPassDrawListContext& Context,
			std::shared_ptr<const FMeshDrawCommand> Existing = {}) const -> FShaderCompletion = 0;
		RENDERER_API auto PrepareShaders(const FMeshPassProcessorBatch& Batch, const FRenderResourceGeneration& Generation,
			const FMaterialCompilerResult* Program, const FShaderCompileOptions& Options, FMeshPassShaders& Out,
			ERenderResourceCreateErrorCategory* FailureCategory = nullptr) const -> FShaderCompletion;
		RENDERER_API auto PreparePipeline(const FMeshPassProcessorBatch& Batch, FName Name,
			const FMeshPassShaders& Shaders, FRHIVertexDeclaration* Declaration, FMeshPassPipeline& Out) const -> FShaderCompletion;
		RENDERER_API auto BuildPSOInitializer(const FMeshPassProcessorBatch& Batch, const FMeshPassShaders& Shaders,
			FRHIVertexDeclaration* Declaration, FGraphicsPipelineStateInitializer& Out) const -> FShaderCompletion;
		RENDERER_API auto BuildGPUCullingCommand(const FMeshPassProcessorBatch& Batch, const FMeshDrawCommand& Prepared,
			FRHIStorageBufferRange VisibleInstances, FRHIStorageBufferRange InstanceTransforms,
			FRHIBuffer* IndirectArgs, uint64 IndirectArgsOffset, FMeshPassDrawListContext& Context) const -> FShaderCompletion;
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

	protected:
		RENDERER_API auto BuildMeshDrawCommands(const FMeshPassProcessorBatch& MeshBatch, const FMeshDrawCommandBuildInput& Input,
			FMeshDrawCommandBindingCache& Cache, FMeshPassDrawListContext& Context,
			std::shared_ptr<const FMeshDrawCommand> Existing) const -> FShaderCompletion;
		RENDERER_API static auto RasterVariants(const FSceneRenderPreparationConfig& Config,
			const FPSOPrecacheParams& Params, uint8 Count, bool bShadow = false, bool bPicking = false)
			-> std::vector<FMeshPSOPrecacheVariant>;
	private:
		struct FSelection
		{
			uint32 MeshPass = 0;
			FMeshPassFragmentShader Fragment;
			FMeshPipelineDescription Description;
		};
		auto SelectMeshPass(const FMeshPassProcessorBatch& Batch, FSelection& Out) const -> FShaderCompletion;
		auto MatchesBatch(const FMeshPassProcessorBatch& Batch, const FMeshDrawDescription& Draw,
			const FVertexFactoryInputBinding& Input) const -> bool;

	};

	using PassProcessorCreateFunction = std::unique_ptr<FMeshPassProcessor> (*)(int32 PSOCollectorIndex);

	// Registration is module-initialization owned; removal requires quiescent users.
	// No concrete pass implementation or creation switch belongs to this manager.
	class FPassProcessorManager
	{
	public:
		RENDERER_API static auto IsRegistered(EMeshPipelinePass Pass) -> bool;
		RENDERER_API static auto GetRegisteredPasses() -> std::vector<EMeshPipelinePass>;
		RENDERER_API static auto GetMeshPassProcessor(EMeshPipelinePass Pass) -> const FMeshPassProcessor&;
		// Each collection request owns a fresh instance and its asynchronous state.
		RENDERER_API static auto CreateMeshPassProcessor(EMeshPipelinePass Pass, int32 PSOCollectorIndex = -1)
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
