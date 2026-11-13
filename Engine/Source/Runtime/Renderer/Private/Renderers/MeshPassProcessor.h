#pragma once

#include "CoreMinimal.h"
#include "PSOPrecacheMaterial.h"
#include "Renderers/GPUSceneRendering.h"
#include "Renderers/MeshPipelineDescription.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "RenderResourceCreation.h"
#include "RendererAPI.h"
#include "Shader/MaterialShader.h"
#include "VertexFactory.h"
#include "Renderers/SurfaceMaterial.h"

namespace Durin
{
	class FPrimitiveSceneInfo;
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
	struct FMeshPipelineIdentity
	{
		FMeshPipelineDescription Description;
		EMeshPipelinePass Pass = EMeshPipelinePass::Forward;
		FXxHash64 FactoryKey, LayoutKey;
		FRHIShader* VertexShader = nullptr;
		FRHIShader* FragmentShader = nullptr;
		FVertexDeclarationElementList VertexElements;
		FPipelineLayoutDesc PipelineLayout;
		uint32 VertexFactoryPass = 0;
		bool bHasShaderMap = false;
		auto operator==(const FMeshPipelineIdentity&) const -> bool = default;
	};
	struct FMeshPassPipeline : FMeshPassShaders
	{
		FVertexDeclarationRHIRef VertexDeclaration;
		FGraphicsPipelineStateRHIRef PipelineState;
		FMeshPipelineDescription Description;
		EMeshPipelinePass Pass = EMeshPipelinePass::Forward;
		auto GetPassKey() const -> FMeshPassKey { return {Pass}; }
		RENDERER_API auto GetIdentity() const -> FMeshPipelineIdentity;
	};
	struct FMeshPassBindings
	{
		FMeshDrawShaderBindings Vertex;
		FMeshDrawShaderBindings Fragment;
	};
	// Submission-scoped resources, never inserted into a retained command.
	struct FMeshDrawCommandOverrideArgs
	{
		FRHIBuffer* InstanceBuffer = nullptr;
		FRHIBuffer* IndirectArgsBuffer = nullptr;
		uint32 InstanceDataByteOffset = 0;
		uint64 IndirectArgsByteOffset = 0;
	};
	struct FMeshDrawCommandSceneArgs
	{
		FRHIBuffer* PrimitiveIdsBuffer = nullptr;
		uint32 InstanceDataByteOffset = 0;
		FRHIBuffer* IndirectArgsBuffer = nullptr;
		uint32 PrimitiveIdOffset = INDEX_NONE_U32; // Durin shader's uint32 ID index.
		uint64 IndirectArgsByteOffset = 0;
		uint32 NumInstances = 0; // Zero preserves the source count.
	};
	// Draw data and bindings only; resource lifetime belongs to the sources.
	struct FMeshDrawCommand
	{
		RENDERER_API auto SetDrawParametersAndFinalize(const FPreparedStaticMeshPrimitive& Primitive, const FPreparedStaticMeshDraw& Draw) -> bool;
		RENDERER_API auto SubmitDraw(FRHICommandList& Commands, FMeshDrawCommandStateCache* Cache = nullptr) const -> bool;
		RENDERER_API auto SubmitVisible(FRHICommandList& Commands, const FMeshDrawShaderBindings& VertexPass, const FMeshDrawShaderBindings& FragmentPass,
			const std::array<float, 3>& Bias, FMeshDrawCommandStateCache* Cache, const FMeshDrawCommandSceneArgs& SceneArgs) const -> bool;
		RENDERER_API auto MatchesForDynamicInstancing(const FMeshDrawCommand& Other) const -> bool;
		RENDERER_API auto GetDynamicInstancingHash() const -> uint32;
		using FVertexStream = FMeshDrawVertexStream;
		std::vector<FVertexStream> VertexStreams;
		FRHIBuffer* IndexBuffer = nullptr;
		uint32 IndexBufferOffset = 0;
		std::variant<FRHIDrawArguments, FRHIDrawIndexedArguments> DrawParameters;
		FRHIBuffer* IndirectArgs = nullptr;
		uint64 IndirectArgsOffset = 0;
		std::array<float, 3> DepthBias{};
		bool bFinalized = false;
		const FMeshPassPipeline* Pipeline = nullptr;
		FMeshDrawShaderBindings VertexBindings, FragmentBindings, VertexPassBindings, FragmentPassBindings;
		bool bCached = false;
		uint32 InstanceIdOffset = INDEX_NONE_U32;
	};
	RENDERER_API auto SetupMeshDrawCommandDynamicInstancing(FRHICommandListImmediate& Commands,
		const FPreparedStaticMeshView& Prepared, FResolvedStaticMeshView& Resolved) -> bool;
	class FCachedMeshDrawCommandStorage;
	struct FCachedMeshDrawCommandInfo
	{
		int32 CommandIndex = INDEX_NONE;
		int32 StateBucketId = INDEX_NONE;
		FMeshPassKey MeshPass;
	};
	struct FVisibleMeshDrawCommand
	{
		const FMeshDrawCommand* Command = nullptr;
		const FMeshPassBindings* Pass = nullptr;
		const FCachedMeshDrawCommandInfo* CachedInfo = nullptr;
		std::array<float, 3> DepthBias{};
		uint32 PrimitiveIdBufferOffset = INDEX_NONE_U32;
		uint32 NumInstances = 0; // Zero preserves the source count.
		bool bSkipDraw = false;
		int32 StateBucketId = INDEX_NONE;
		FGPUScenePrimitiveData SceneData;
		bool bSupportsInstanceCulling = false;
		FRHIBuffer* InstanceBuffer = nullptr;
		bool bSupportsDynamicInstancing = false;
		auto SubmitDraw(FRHICommandList& Commands, FMeshDrawCommandStateCache* Cache = nullptr) const -> bool
		{
			if (bSkipDraw) return true;
			return Command && Pass && (!CachedInfo || CachedInfo->CommandIndex != INDEX_NONE)
				&& Command->SubmitVisible(Commands, Pass->Vertex, Pass->Fragment, DepthBias, Cache,
					{.PrimitiveIdsBuffer = InstanceBuffer, .PrimitiveIdOffset = PrimitiveIdBufferOffset, .NumInstances = NumInstances});
		}
	};
	using FMeshCommandOneFrameArray = std::vector<FVisibleMeshDrawCommand>;
	using FMeshDrawCommandRun = std::vector<uint32>;
	RENDERER_API auto BuildMeshDrawCommandRuns(std::span<const FVisibleMeshDrawCommand> Visible) -> std::vector<FMeshDrawCommandRun>;
	// Adapter from collection/preparation into the common mesh-command boundary.
	RENDERER_API auto CollectMeshDrawCommands(const FPreparedStaticMeshView& Prepared,
		const FResolvedStaticMeshView& Resolved, FMeshPassKey Pass, bool bAllowDynamicInstancing = true) -> FMeshCommandOneFrameArray;

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
		FRHIStorageBufferRange LocalLights{};
		FGPUSceneShaderParameters GPUScene;
		uint32 InstanceIdOffset = INDEX_NONE_U32;
	};
	// One view-preparation scope reuses validated binding values.
	struct FMeshDrawCommandBindingCache
	{
		using FBindings = FMeshDrawShaderBindings;
		std::map<std::tuple<uint32, EMeshPipelinePass, uint32, FRHIShader*>, FBindings> Fragments;
		using FPassKey = std::tuple<EMeshPipelinePass, FRHIShader*, FRHIBuffer*>;
		std::map<FPassKey, FBindings> Passes;
		std::map<std::tuple<uint32, uint32, EMeshPipelinePass, FRHIShader*>, FMeshDrawShaderBindings> Vertices;
		std::map<std::pair<const FMeshPassPipeline*, FRHIBuffer*>, std::shared_ptr<const FMeshPassBindings>> PassGroups;
		size_t PreparedSurfaceBindingBatches = 0, SurfaceBindingBatchReuses = 0;
	};
	// Cache metadata belongs to storage, never to the RHI draw description.
	class FCachedMeshDrawCommandStorage final : public FRenderResource
	{
	public:
		struct FKey
		{
			std::array<uint64, 5> Mesh;
			std::array<uint64, 2> Bindings;
			FMeshPipelineIdentity Pipeline;
		};
		RENDERER_API static auto MakeKey(const FMeshDrawDescription* Description, FPrimitiveComponentId PrimitiveId,
			uint64 BatchId, const FMeshPassPipeline& Pipeline, const FRHIResource* MaterialUniform = nullptr) -> FKey;
		FCachedMeshDrawCommandStorage() = default;
		RENDERER_API auto ReleaseRHI() -> void override;
		RENDERER_API static auto ReleaseRHIForAllResources() -> void;
		auto GetFriendlyName() const -> std::string override { return "CachedMeshDrawCommands"; }
		RENDERER_API ~FCachedMeshDrawCommandStorage();
		FCachedMeshDrawCommandStorage(const FCachedMeshDrawCommandStorage&) = delete;
		auto operator=(const FCachedMeshDrawCommandStorage&) -> FCachedMeshDrawCommandStorage& = delete;
		RENDERER_API auto Attach(FPrimitiveSceneInfo& Owner) -> void;
		RENDERER_API auto Find(const FKey& Key, const FPrimitiveSceneInfo& Owner,
			FResolvedStaticMeshView::FMaterialUniform* Material = nullptr, const FVertexFactoryInputBinding* Input = nullptr) const -> std::shared_ptr<const FMeshDrawCommand>;
		RENDERER_API auto FindInfo(const FKey& Key, const FPrimitiveSceneInfo& Owner) const -> std::shared_ptr<const FCachedMeshDrawCommandInfo>;
		RENDERER_API auto Publish(const FKey& Key, FMeshDrawCommand Command, FPrimitiveSceneInfo& Owner,
			const FVertexFactoryInputBinding* Input = nullptr, FResolvedStaticMeshView::FMaterialUniform Material = {},
			std::shared_ptr<const FMeshPassPipeline> Pipeline = {}) -> std::shared_ptr<const FMeshDrawCommand>;
		RENDERER_API auto RemovePrimitive(FPrimitiveSceneInfo& Owner) -> void;
		RENDERER_API auto Detach(FPrimitiveSceneInfo& Owner) -> void;
		RENDERER_API auto Reset() -> void;
		auto Num() const -> size_t { return Entries.size() - FreeIndices.size(); }
		FRenderResourceGeneration Generation;
	private:
		struct FStateBucket
		{
			std::shared_ptr<const FMeshDrawCommand> Command;
			std::shared_ptr<const FMeshPassPipeline> Pipeline;
			int32 Id = INDEX_NONE;
		};
		struct FEntry
		{
			std::shared_ptr<const FMeshDrawCommand> Command;
			std::shared_ptr<const FStateBucket> StateBucket;
			std::shared_ptr<FCachedMeshDrawCommandInfo> Info;
			std::array<uint64, 2> Bindings;
			FMeshPipelineIdentity Pipeline;
			const FVertexFactoryInputBinding* Input = nullptr;
			FResolvedStaticMeshView::FMaterialUniform Material;
		};
		auto FindEntry(const FKey& Key, const FPrimitiveSceneInfo& Owner) const -> const FEntry*;
		auto RemoveEntry(size_t Index) -> void;
		std::unordered_map<uint32, std::vector<std::weak_ptr<const FStateBucket>>> StateBuckets;
		int32 NextStateBucketId = 0;
		std::vector<std::unique_ptr<FEntry>> Entries;
		std::vector<size_t> FreeIndices;
		std::unordered_set<FPrimitiveSceneInfo*> Sources;
	};
	// Stable-address frame storage. Aliasing handles retain the entire frame allocation
	// through resolved frame storage, including replaced indirect command values.
	class FDynamicMeshDrawCommandStorage
	{
	public:
		RENDERER_API FDynamicMeshDrawCommandStorage();
		FDynamicMeshDrawCommandStorage(const FDynamicMeshDrawCommandStorage&) = delete;
		auto operator=(const FDynamicMeshDrawCommandStorage&) -> FDynamicMeshDrawCommandStorage& = delete;
		RENDERER_API auto Publish(FMeshPassKey Pass, FMeshDrawCommand Command,
			FMeshDrawCommandHandle Replace = {}) -> FMeshDrawCommandHandle;
		RENDERER_API auto GetCommand(FMeshDrawCommandHandle Handle) const -> std::shared_ptr<const FMeshDrawCommand>;
		RENDERER_API auto PublishVisible(std::shared_ptr<const FMeshDrawCommand> Command,
			std::shared_ptr<const FCachedMeshDrawCommandInfo> Info, std::shared_ptr<const FMeshPassBindings> Pass, std::array<float, 3> Bias, uint32 InstanceIdOffset = INDEX_NONE_U32, uint32 InstanceCount = 0, bool bSkipDraw = false, int32 StateBucketId = INDEX_NONE) -> FMeshDrawCommandHandle;
		RENDERER_API auto GetVisible(FMeshDrawCommandHandle Handle) const -> const FVisibleMeshDrawCommand*;
		// Command is already retained by this frame's cached or dynamic list.
		RENDERER_API auto PublishVisible(FVisibleMeshDrawCommand Visible, std::shared_ptr<const FMeshPassBindings> Pass) -> FMeshDrawCommandHandle;
		RENDERER_API auto Num(FMeshPassKey Pass) const -> size_t;
	private:
		struct FData
		{
			uint64 Id = 0;
			uint64 Frame = 0;
			std::vector<std::shared_ptr<const FMeshDrawCommand>> RetainedCommands;
			std::vector<std::shared_ptr<const FCachedMeshDrawCommandInfo>> RetainedInfos;
			std::vector<std::shared_ptr<const FMeshPassBindings>> RetainedPasses;
			std::map<FMeshPassKey, std::vector<FVisibleMeshDrawCommand>> Visible;
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
			std::shared_ptr<const FMeshDrawCommand>& Out, const FMeshDrawCommandBuildInput& InInput)
			: Storage(InStorage), Pass(InPass), Output(Out), Input(InInput) {}
		RENDERER_API auto FinalizeCommand() -> void override;
		auto IsCached() const -> bool override { return true; }
	private:
		FCachedMeshDrawCommandStorage& Storage;
		FMeshPassKey Pass;
		std::shared_ptr<const FMeshDrawCommand>& Output;
		const FMeshDrawCommandBuildInput& Input;
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
	class FMeshPSOPrecacheResources;
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
		RENDERER_API auto PreparePassBindings(const FMeshDrawCommandBuildInput& Input,
			FMeshDrawCommandBindingCache& Cache, FMeshPassBindings& Out) const -> FShaderCompletion;

		virtual auto IsEnabled(const FSceneRenderPreparationConfig& Config) const -> bool = 0;
		RENDERER_API auto IsMeshRelevant(const FMaterialPlanningPassIdentity& Material,
			const FPrimitiveRenderParticipation& Participation = {}, bool bSupportsGBuffer = true) const -> bool;
		RENDERER_API auto SupportsVertexFactory(const FMeshVertexFactoryImplementation& Factory) const -> bool;
		virtual auto GetVertexFactoryPass() const -> uint32 = 0;
		virtual auto GetFragmentShader(FMaterialShaderBlendModeKey BlendMode) const -> FMeshPassFragmentShader = 0;
		virtual auto DescribeMeshDraw(const FMeshPipelineState& State,
			FGraphicsPipelineStateInitializer::EPrimitiveTopology Topology,
			FMaterialShaderBlendModeKey BlendMode = EMaterialBlendMode::Opaque, bool bReversedZ = false) const -> FMeshPipelineDescription = 0;
		virtual auto GetPSOPrecacheVariants(const FSceneRenderPreparationConfig& Config,
			const FPSOPrecacheParams& Params) const -> std::vector<FMeshPSOPrecacheVariant> = 0;

		RENDERER_API auto PreparePSOResources(const FMaterialPSOPrecacheParams& Params,
			const FRenderResourceGeneration& Generation, bool bRequired) -> std::expected<EPSOCollectorPreparation, std::string> override;
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
		std::unique_ptr<FMeshPSOPrecacheResources> PSOResources;
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
