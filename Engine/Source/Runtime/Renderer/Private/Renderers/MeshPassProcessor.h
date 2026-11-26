#pragma once

#include "CoreMinimal.h"
#include "PipelineStateCache.h"
#include "Misc/EnumClassFlags.h"
#include "Renderers/MeshDrawCommandSubmission.h"
#include "Renderers/MeshDrawCommandStats.h"
#include "PSOPrecacheMaterial.h"
#include "Renderers/GPUSceneRendering.h"
#include "Renderers/MeshPipelineDescription.h"
#include "Renderers/MeshRenderPreparationCommon.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "RenderResourceCreation.h"
#include "RendererAPI.h"
#include "Shader/MaterialShader.h"
#include "VertexFactory.h"
#include "Renderers/SurfaceMaterial.h"

namespace Durin
{
	class FPrimitiveSceneInfo;
	class FInstanceCullingContext;
	class FInstanceCullingManager;
	struct FInstanceCullingAdmissionDiagnostics;
	class FShaderType;
	class FMeshMaterialShaderMap;
}

namespace Durin::RendererPrivate
{
	struct FStaticMeshCommandBuildContext;
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
		FVertexDeclarationElementList VertexElements{};
		FRHIMultisampleState Multisample;
		FPipelineLayoutDesc PipelineLayout;
		uint32 VertexFactoryPass = 0;
		bool bHasShaderMap = false;
		bool bHasFixedState = false;
		auto operator==(const FMeshPipelineIdentity&) const -> bool = default;
	};
	// Mutable construction input; publishing copies/moves it into immutable storage.
	struct FMeshPassPipelineInitializer : FMeshPassShaders
	{
		FVertexDeclarationRHIRef VertexDeclaration;
		std::optional<FGraphicsPipelineStateInitializer> PipelineInitializer;
		FMeshPipelineDescription Description;
		EMeshPipelinePass Pass = EMeshPipelinePass::Forward;
	};
	struct FMeshPassPipeline final
	{
		RENDERER_API explicit FMeshPassPipeline(FMeshPassPipelineInitializer InInitializer = {});
		auto GetInitializer() const -> const FMeshPassPipelineInitializer& { return Initializer; }
		auto GetPassKey() const -> FMeshPassKey { return {Initializer.Pass}; }
		auto GetIdentity() const -> const std::shared_ptr<const FMeshPipelineIdentity>& { return Identity; }
	private:
		const FMeshPassPipelineInitializer Initializer;
		const std::shared_ptr<const FMeshPipelineIdentity> Identity;
	};
	struct FMeshPassBindings
	{
		FMeshDrawShaderBindings Vertex;
		FMeshDrawShaderBindings Fragment;
	};
	// Each shader-map slot tracks only variants still owned by draws.
	struct FCachedMeshPassShaders : FMeshPassShaders
	{
		std::vector<std::weak_ptr<const FMeshPassPipeline>> Pipelines;
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
		RENDERER_API auto SetDrawParameters(const FPreparedStaticMeshPrimitive& Primitive, const FPreparedStaticMeshDraw& Draw) -> bool;
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
		FMeshDrawShaderBindings VertexBindings, FragmentBindings;
		FMeshPassBindings PassBindings;
		bool bCached = false;
		uint32 InstanceIdOffset = INDEX_NONE_U32;
	};
	class FCachedMeshDrawCommandStorage;
	struct FMeshDrawCommandCacheReadScope;
	struct FCachedMeshDrawCommandInfo
	{
		const FCachedMeshDrawCommandStorage* Storage = nullptr;
		int32 CommandIndex = INDEX_NONE;
		int32 StateBucketId = INDEX_NONE;
		FMeshPassKey MeshPass;
	};
	struct FVisibleMeshDrawCommand
	{
		const FMeshDrawCommand* Command = nullptr;
		const FMeshPassBindings* Pass = nullptr;
		std::array<float, 3> DepthBias{};
		uint32 PrimitiveIdBufferOffset = INDEX_NONE_U32;
		uint32 NumInstances = 0; // Zero preserves the source count.
		uint32 SourceDrawIndex = INDEX_NONE_U32;
		FRHIBuffer* InstanceBuffer = nullptr;
		uint32 NumSourceDraws = 1;
		EMeshBasePass BasePass = EMeshBasePass::Opaque;
		bool bSourceReady = true;
		RENDERER_API auto MatchesForDynamicInstancing(const FVisibleMeshDrawCommand& Other) const -> bool;
		auto SubmitDraw(FRHICommandList& Commands, FMeshDrawCommandStateCache* Cache = nullptr,
			const FMeshDrawCommandOverrideArgs& Override = {},
			const FMeshDrawShaderBindings* FragmentOverride = nullptr) const -> bool
		{
			if (!bSourceReady || !Command || !Pass) return false;
			// Generated IDs apply only to admitted indirect draws. Direct and CPU-merged
			// fallbacks retain this command's ID stream and logical instance offset.
			const bool bIndirect = Override.IndirectArgsBuffer != nullptr;
			if (bIndirect && (!Override.InstanceBuffer || !Command->IndexBuffer)) return false;
			return Command->SubmitVisible(Commands, Pass->Vertex, FragmentOverride ? *FragmentOverride : Pass->Fragment, DepthBias, Cache,
				{.PrimitiveIdsBuffer = bIndirect ? Override.InstanceBuffer : InstanceBuffer,
					.InstanceDataByteOffset = bIndirect ? Override.InstanceDataByteOffset : 0,
					.IndirectArgsBuffer = Override.IndirectArgsBuffer,
					.PrimitiveIdOffset = bIndirect ? INDEX_NONE_U32 : PrimitiveIdBufferOffset,
					.IndirectArgsByteOffset = Override.IndirectArgsByteOffset,
					.NumInstances = bIndirect ? 0 : NumInstances});
		}
	};
	// Functional submission result; independent of optional feature statistics.
	struct FMeshDrawCommandPassResult
	{
		size_t Attempted = 0, Successful = 0, Rejected = 0, NativeSubmissions = 0, IndirectSubmissions = 0;
		bool bComplete = true, bRenderedGeometry = false;
		RENDERER_API auto Record(uint32 NumSourceDraws, bool bSuccess, uint32 NativeCalls, bool bIndirect) -> void;
	};
	// Source-indexed preparation input. Never retained in final visible/fallback commands.
	struct FMeshDrawCommandCullingPayload
	{
		FGPUScenePrimitiveData SceneData;
		bool bSupportsDynamicInstancing = false;
		bool bSupportsInstanceCulling = false;
	};
	RENDERER_API auto BuildMeshDrawCommandCullingPayloads(const FPreparedStaticMeshView& Prepared,
		const FResolvedStaticMeshView& Resolved, FMeshPassKey Pass) -> std::vector<FMeshDrawCommandCullingPayload>;
	using FMeshCommandOneFrameArray = std::vector<FVisibleMeshDrawCommand>;
	RENDERER_API auto BuildMeshDrawCommandRuns(std::span<const FVisibleMeshDrawCommand> Visible, std::span<const FMeshDrawCommandCullingPayload> Sources) -> std::vector<FMeshDrawCommandRun>;
	// Preparation owns publication and in-place compaction. Recording only borrows spans.
	RENDERER_API auto SetupMeshDrawCommands(FRHICommandListImmediate& Commands,
		const FPreparedStaticMeshView& Prepared, FResolvedStaticMeshView& Resolved, bool bAllowDynamicInstancing) -> bool;
	// One prepared pass list and its source mapping; resources are retained once per owner.
	struct FParallelMeshDrawCommandPass
	{
		explicit FParallelMeshDrawCommandPass(FMeshPassKey InPass = {}) : Pass(InPass) {}
		FParallelMeshDrawCommandPass(const FParallelMeshDrawCommandPass&) = delete;
		auto operator=(const FParallelMeshDrawCommandPass&) -> FParallelMeshDrawCommandPass& = delete;
		auto GetPass() const -> FMeshPassKey { return Pass; }
		// Cached visible records can be borrowed only inside this pass's scene-cache read phase.
		RENDERER_API auto BeginRecording() -> bool;
		auto GetMeshDrawCommands() const -> std::span<const FVisibleMeshDrawCommand>
		{
			if (!RetainedCachedDraws.empty() && !CacheReadScope) return {};
			return VisibleMeshDrawCommands;
		}
		auto ReserveCachedCommands(size_t Count) -> void
		{
			VisibleMeshDrawCommands.reserve(Count);
			RetainedCachedDraws.reserve(Count);
		}
		auto GetBasePassRange(EMeshBasePass Bucket) const -> FMeshDrawCommandRun { return BasePassRanges[static_cast<uint32>(Bucket)]; }
		RENDERER_API auto AddCachedMeshDrawCommand(FCachedMeshDrawCommand Cached, std::shared_ptr<const FMeshPassBindings> Bindings,
			std::array<float, 3> Bias, uint32 InstanceIdOffset = INDEX_NONE_U32, uint32 SourceDrawIndex = INDEX_NONE_U32) -> void;
		RENDERER_API auto AddMeshDrawCommand(std::shared_ptr<const FMeshDrawCommand> Command, uint32 SourceDrawIndex) -> void;
		RENDERER_API auto GetVisibleForSource(uint32 SourceDrawIndex) const -> const FVisibleMeshDrawCommand*;
		RENDERER_API auto SetupDrawCommands(FInstanceCullingManager& Manager, uint32 ViewId, std::span<const FMeshDrawCommandCullingPayload> Sources,
			size_t* Overflow, size_t Budget, FInstanceCullingAdmissionDiagnostics* Diagnostics) -> bool;
		auto GetInstanceCullingContext() -> FInstanceCullingContext* { return InstanceCullingContext.get(); }
		auto GetInstanceCullingContext() const -> const FInstanceCullingContext* { return InstanceCullingContext.get(); }
		RENDERER_API auto Draw(FRHICommandList& Commands, const FMeshDrawCommandOverrideArgs& OverrideArgs = {},
			FMeshDrawCommandStateCache* Cache = nullptr, FMeshDrawCommandPassStats* Statistics = nullptr) const -> FMeshDrawCommandPassResult;
		RENDERER_API auto Draw(FRHICommandList& Commands, uint32 StartIndex, uint32 NumMeshDrawCommands,
			const FMeshDrawCommandOverrideArgs& OverrideArgs = {}, FMeshDrawCommandStateCache* Cache = nullptr, FMeshDrawCommandPassStats* Statistics = nullptr) const -> FMeshDrawCommandPassResult;
		// Null selects the ordinary serial Draw path. Non-null partitions the final
		// visible commands, including compacted instance-culling commands, in order.
		RENDERER_API auto DispatchDraw(FParallelCommandListSet* ParallelCommandListSet, FRHICommandList& Commands,
			const FMeshDrawCommandOverrideArgs& OverrideArgs = {}, FMeshDrawCommandPassStats* Statistics = nullptr) const -> FMeshDrawCommandPassResult;
		RENDERER_API auto DispatchDraw(FParallelCommandListSet* ParallelCommandListSet, FRHICommandList& Commands,
			uint32 StartIndex, uint32 NumMeshDrawCommands, const FMeshDrawCommandOverrideArgs& OverrideArgs = {},
			FMeshDrawCommandPassStats* Statistics = nullptr) const -> FMeshDrawCommandPassResult;
	private:
		friend struct FMeshDrawCommandPassTestAccess;
		friend RENDERER_API auto SetupMeshDrawCommands(FRHICommandListImmediate&, const FPreparedStaticMeshView&, FResolvedStaticMeshView&, bool) -> bool;
		// Each pass owns selection and retained cache metadata; dynamic commands share one allocation owner.
		struct FRetainedCachedDraw
		{
			FCachedMeshDrawCommand Cached;
			std::shared_ptr<const FMeshPassBindings> Bindings;
		};
		std::vector<FRetainedCachedDraw> RetainedCachedDraws;
		std::shared_ptr<const FMeshDrawCommandCacheReadScope> CacheReadScope;
		std::shared_ptr<const void> DynamicStorageOwner;
		uint64 RenderFrame = GRenderFrameCounterRenderThread;
		FMeshCommandOneFrameArray VisibleMeshDrawCommands;
		std::vector<uint32> SourceCommandIndices;
		std::vector<std::shared_ptr<const FGPUSceneBufferSet>> SceneDataOwners;
		FBufferRHIRef InstanceIdBuffer;
		std::array<FMeshDrawCommandRun, 3> BasePassRanges{};
		bool bFinalized = false;
		FMeshPassKey Pass;
		std::shared_ptr<FInstanceCullingContext> InstanceCullingContext;
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
		FRHIStorageBufferRange LocalLights{};
		FGPUSceneShaderParameters GPUScene;
		uint32 InstanceIdOffset = INDEX_NONE_U32;
	};
	// Scene publication consumes only the stable bindings in Input.
	struct FStaticMeshCommandBuildRequest
	{
		FMeshDrawCommandBuildInput Input;
		FCachedMeshDrawCommand& Result;
	};
	auto MakeMeshPassProcessorBatch(const FMeshDrawCommandBuildInput& Input,
		const FMeshVertexFactoryImplementation& Factory) -> FMeshPassProcessorBatch;
	// One view-preparation scope reuses validated binding values.
	struct FMeshDrawCommandBindingCache
	{
		using FBindings = FMeshDrawShaderBindings;
		std::map<std::tuple<uint32, EMeshPipelinePass, uint32, FRHIShader*>, FBindings> Fragments;
		using FPassKey = std::tuple<EMeshPipelinePass, FRHIShader*, FRHIBuffer*>;
		std::map<FPassKey, FBindings> Passes;
		std::map<std::tuple<uint32, uint32, EMeshPipelinePass, FRHIShader*>, FMeshDrawShaderBindings> Vertices;
		using FPassGroupKey = std::tuple<EMeshPipelinePass, const FMeshVertexShaderBinding*, const FCompiledSurfaceBindingLayout*, FRHIBuffer*>;
		std::map<FPassGroupKey, std::shared_ptr<const FMeshPassBindings>> PassGroups;
		size_t PreparedSurfaceBindingBatches = 0, SurfaceBindingBatchReuses = 0;
	};
	// Cache metadata belongs to storage, never to the RHI draw description.
	// Each finalized pass shares this scene-owned token until its recording data is destroyed.
	struct FMeshDrawCommandCacheReadScope {};
	class FCachedMeshDrawCommandStorage final : public FRenderResource
	{
	public:
		struct FStaticMeshLocation
		{
			std::shared_ptr<const FStaticMeshBatchList> List;
			uint32 MeshIndex;
			FStaticMeshCommandVariant Variant;
		};
		using FDynamicMeshKey = std::array<uint64, 7>;
		struct FKey
		{
			std::variant<FStaticMeshLocation, FDynamicMeshKey> Mesh;
			std::array<uint64, 2> Bindings;
			std::shared_ptr<const FMeshPipelineIdentity> Pipeline;
		};
		RENDERER_API static auto MakeKey(const FPreparedStaticMeshDraw& Draw, const FPreparedStaticMeshPrimitive& Primitive, const FMeshPassPipeline& Pipeline, const FRHIResource* MaterialUniform = nullptr) -> FKey;
		FCachedMeshDrawCommandStorage() = default;
		RENDERER_API auto ReleaseRHI() -> void override;
		RENDERER_API static auto ReleaseRHIForAllResources() -> void;
		static auto InvalidateMeshPreparationForAllResources(bool bUpdateStaticMeshElements = false) -> void;
		bool bNeedsStaticMeshElementUpdate = false;
		auto GetFriendlyName() const -> std::string override { return "CachedMeshDrawCommands"; }
		RENDERER_API ~FCachedMeshDrawCommandStorage();
		FCachedMeshDrawCommandStorage(const FCachedMeshDrawCommandStorage&) = delete;
		auto operator=(const FCachedMeshDrawCommandStorage&) -> FCachedMeshDrawCommandStorage& = delete;
		RENDERER_API auto Attach(FPrimitiveSceneInfo& Owner) -> void;
		RENDERER_API auto FindCachedCommand(const FKey& Key, const FPrimitiveSceneInfo& Owner,
			FResolvedStaticMeshView::FMaterialUniform* Material = nullptr, const FVertexFactoryInputBinding* Input = nullptr) const
			-> FCachedMeshDrawCommand;
		RENDERER_API auto Publish(const FKey& Key, FMeshDrawCommand Command, FPrimitiveSceneInfo& Owner,
			std::shared_ptr<const FMeshPassPipeline> Pipeline,
			const FVertexFactoryInputBinding* Input = nullptr, FResolvedStaticMeshView::FMaterialUniform Material = {}) -> FCachedMeshDrawCommand;
		RENDERER_API auto RemovePrimitive(FPrimitiveSceneInfo& Owner) -> void;
		RENDERER_API auto Detach(FPrimitiveSceneInfo& Owner) -> void;
		RENDERER_API auto Reset() -> void;
		auto Num() const -> size_t { return Entries.size() - FreeIndices.size(); }
		FRenderResourceGeneration Generation;
		FMeshDrawDescriptionCache MeshPreparation;
	private:
		friend struct FParallelMeshDrawCommandPass;
		const std::shared_ptr<const FMeshDrawCommandCacheReadScope> ReadScope = std::make_shared<FMeshDrawCommandCacheReadScope>();
		struct FStateBucket
		{
			std::shared_ptr<const FMeshDrawCommand> Command;
			std::shared_ptr<const FMeshPassPipeline> Pipeline;
			int32 Id = INDEX_NONE;
		};
		struct FEntry
		{
			std::shared_ptr<const FStateBucket> StateBucket;
			std::shared_ptr<FCachedMeshDrawCommandInfo> Info;
			std::array<uint64, 2> Bindings;
			std::shared_ptr<const FMeshPipelineIdentity> Pipeline;
			const FVertexFactoryInputBinding* Input = nullptr;
			FResolvedStaticMeshView::FMaterialUniform Material;
		};
		auto FindEntry(const FKey& Key, const FPrimitiveSceneInfo& Owner) const -> const FEntry*;
		auto RequireWritable() const -> void;
		auto RemoveEntry(size_t Index) -> void;
		std::unordered_map<uint32, std::vector<std::weak_ptr<const FStateBucket>>> StateBuckets;
		int32 NextStateBucketId = 0;
		std::vector<std::unique_ptr<FEntry>> Entries;
		std::vector<size_t> FreeIndices;
		std::unordered_set<FPrimitiveSceneInfo*> Sources;
	};
	// Append-only frame storage. Published command pointers retain the stable allocation.
	class FDynamicMeshDrawCommandStorage
	{
	public:
		RENDERER_API FDynamicMeshDrawCommandStorage();
		FDynamicMeshDrawCommandStorage(const FDynamicMeshDrawCommandStorage&) = delete;
		auto operator=(const FDynamicMeshDrawCommandStorage&) -> FDynamicMeshDrawCommandStorage& = delete;
	private:
		friend class FDynamicPassMeshDrawListContext;
		struct FData
		{
			uint64 Frame = 0;
			std::deque<FMeshDrawCommand> Commands;
		};
		std::shared_ptr<FData> Data;
	};
	// Contexts own build-scope destinations and remain alive across processor use.
	class FMeshPassDrawListContext
	{
	public:
		virtual ~FMeshPassDrawListContext() = default;
		explicit FMeshPassDrawListContext(FMeshDrawCommandBindingCache& InCache, FStaticMeshCommandBuildContext* InStaticContext = nullptr)
			: Cache(InCache), StaticContext(InStaticContext) {}
		FMeshPassDrawListContext(const FMeshPassDrawListContext&) = delete;
		auto operator=(const FMeshPassDrawListContext&) -> FMeshPassDrawListContext& = delete;
		// Add selects writable build storage; Finalize publishes that command once.
		// Only finalize references returned by this context; stop mutating after publication.
		virtual auto AddCommand(FMeshDrawCommand& Initializer) -> FMeshDrawCommand& = 0;
		virtual auto FinalizeCommand(const FMeshDrawCommandBuildInput& Input, FMeshDrawCommand& Command) -> void = 0;
		virtual auto IsCached() const -> bool = 0;
	private:
		friend class FMeshPassProcessor;
		FMeshDrawCommandBindingCache& Cache;
		FStaticMeshCommandBuildContext* StaticContext;
	};
	class FCachedPassMeshDrawListContext final : public FMeshPassDrawListContext
	{
	public:
		FCachedPassMeshDrawListContext(FCachedMeshDrawCommandStorage& InStorage, FMeshPassKey InPass, FMeshDrawCommandBindingCache& Cache, FStaticMeshCommandBuildContext* StaticContext = nullptr)
			: FMeshPassDrawListContext(Cache, StaticContext), Storage(InStorage), Pass(InPass) {}
		// Each accepted pipeline variant finalizes one command for this pass.
		auto AddCommand(FMeshDrawCommand& Initializer) -> FMeshDrawCommand& override { Initializer.bFinalized = false; return Initializer; }
		RENDERER_API auto FinalizeCommand(const FMeshDrawCommandBuildInput& Input, FMeshDrawCommand& Command) -> void override;
		auto IsCached() const -> bool override { return true; }
		auto GetCommandInfoAndReset() -> FCachedMeshDrawCommand { return std::exchange(CommandInfo, {}); }
	private:
		FCachedMeshDrawCommandStorage& Storage;
		FMeshPassKey Pass;
		FCachedMeshDrawCommand CommandInfo;
	};
	class FDynamicPassMeshDrawListContext : public FMeshPassDrawListContext
	{
	public:
		FDynamicPassMeshDrawListContext(FDynamicMeshDrawCommandStorage& InStorage, FParallelMeshDrawCommandPass& InPass, FMeshDrawCommandBindingCache& Cache)
			: FMeshPassDrawListContext(Cache), Storage(InStorage), Pass(InPass) {}
		RENDERER_API auto AddCommand(FMeshDrawCommand& Initializer) -> FMeshDrawCommand& override;
		RENDERER_API auto FinalizeCommand(const FMeshDrawCommandBuildInput& Input, FMeshDrawCommand& Command) -> void override;
		auto IsCached() const -> bool override { return false; }
	private:
		FDynamicMeshDrawCommandStorage& Storage;
		FParallelMeshDrawCommandPass& Pass;
	};


	// One pass owns eligibility, shader selection and pipeline description for both
	// drawing and collection. Geometry visibility and native submission stay outside.
	class FMeshPSOPrecacheResources;
	class FMeshPassProcessor : public IPSOCollector
	{
	public:
		RENDERER_API explicit FMeshPassProcessor(int32 InPSOCollectorIndex = -1);
		RENDERER_API ~FMeshPassProcessor() override;
		// The caller owns the context and keeps it alive for all AddMeshBatch calls.
		auto SetDrawListContext(FMeshPassDrawListContext* Context) -> void { DrawListContext = Context; }
		virtual auto GetPass() const -> EMeshPipelinePass = 0;
		RENDERER_API virtual auto AddMeshBatch(const FMeshBatch& MeshBatch, uint64 BatchElementMask,
			const FPrimitiveSceneProxy* PrimitiveSceneProxy, int32 StaticMeshId = INDEX_NONE) -> void;
		RENDERER_API auto BuildMeshDrawCommands(const FMeshPassProcessorBatch& Batch, const FMeshDrawCommandBuildInput& Input,
			std::shared_ptr<const FMeshDrawCommand> Existing = {}) const -> FShaderCompletion;
		RENDERER_API auto PrepareShaders(const FMeshPassProcessorBatch& Batch,
			const FMaterialProgram* Program, const FShaderCompileOptions& Options, FMeshPassShaders& Out,
			ERenderResourceCreateErrorCategory* FailureCategory = nullptr) const -> FShaderMapQueryResult;
		static auto PrecachePipelineIfRequested(const FGraphicsPipelineStateInitializer& Initializer) -> FShaderCompletion;
		auto ResolvePipeline(const FMeshPassProcessorBatch& Batch, FCachedMeshPassShaders& Shaders,
			FRHIVertexDeclaration* Declaration) const -> std::shared_ptr<const FMeshPassPipeline>;
		RENDERER_API auto PreparePipeline(const FMeshPassProcessorBatch& Batch,
			const FMeshPassShaders& Shaders, FRHIVertexDeclaration* Declaration, std::shared_ptr<const FMeshPassPipeline>& Out) const -> FShaderCompletion;
		RENDERER_API auto BuildPSOInitializer(const FMeshPassProcessorBatch& Batch, const FMeshPassShaders& Shaders,
			FRHIVertexDeclaration* Declaration, FGraphicsPipelineStateInitializer& Out) const -> FShaderCompletion;
		RENDERER_API auto PreparePassBindings(const FMeshDrawCommandBuildInput& Input,
			FMeshDrawCommandBindingCache& Cache, FMeshPassBindings& Out) const -> FShaderCompletion;

		virtual auto IsMaterialRelevant(const FMaterialPlanningPassIdentity&, bool) const -> bool { return true; }
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
			const FMaterialProgram& Program, const FMaterialStaticProperties& Properties, const FPSOPrecacheVertexFactoryData& VertexFactoryData,
			const FPSOPrecacheParams& PrecacheParams, FPSOPrecacheDataArray& PSOInitializers) -> void override;

	protected:
		auto ProcessStaticMeshBatch(const FMeshBatch& MeshBatch, uint64 BatchElementMask,
			const FPrimitiveSceneProxy* PrimitiveSceneProxy, int32 StaticMeshId, EMeshCollectionPurpose Purpose) const -> void;
		RENDERER_API static auto RasterVariants(const FSceneRenderPreparationConfig& Config,
			const FPSOPrecacheParams& Params, uint8 Count, bool bShadow = false, bool bPicking = false)
			-> std::vector<FMeshPSOPrecacheVariant>;
	private:
		FMeshPassDrawListContext* DrawListContext = nullptr;
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
	enum class EMeshPassFlags : uint8 { None = 0, CachedMeshCommands = 1 };

	ENUM_CLASS_FLAGS(EMeshPassFlags)

	class FPassProcessorManager
	{
	public:
		RENDERER_API static auto GetPassFlags(EShadingPath ShadingPath, EMeshPipelinePass Pass) -> EMeshPassFlags;
		RENDERER_API static auto GetPSOCollectorIndex(EShadingPath ShadingPath, EMeshPipelinePass Pass) -> int32;
		RENDERER_API static auto IsRegistered(EShadingPath ShadingPath, EMeshPipelinePass Pass) -> bool;
		RENDERER_API static auto GetRegisteredPasses(EShadingPath ShadingPath) -> std::vector<EMeshPipelinePass>;
		RENDERER_API static auto GetMeshPassProcessor(EShadingPath ShadingPath, EMeshPipelinePass Pass) -> const FMeshPassProcessor&;
		// Each collection request owns a fresh instance and its asynchronous state.
		RENDERER_API static auto CreateMeshPassProcessor(EShadingPath ShadingPath, EMeshPipelinePass Pass, FMeshPassDrawListContext* Context = nullptr)
			-> std::unique_ptr<FMeshPassProcessor>;
	};

	class FRegisterPassProcessorCreateFunction
	{
	public:
		RENDERER_API FRegisterPassProcessorCreateFunction(PassProcessorCreateFunction CreateFunction, EShadingPath ShadingPath, EMeshPipelinePass Pass,
			EMeshPassFlags Flags, int32 PSOCollectorIndex = -1);
		RENDERER_API ~FRegisterPassProcessorCreateFunction();
		FRegisterPassProcessorCreateFunction(const FRegisterPassProcessorCreateFunction&) = delete;
		auto operator=(const FRegisterPassProcessorCreateFunction&) -> FRegisterPassProcessorCreateFunction& = delete;
		auto IsRegistered() const -> bool { return bRegistered; }
	private:
		PassProcessorCreateFunction CreateFunction;
		EShadingPath ShadingPath;
		EMeshPipelinePass Pass;
		bool bRegistered = false;
	};
}
