#pragma once

#include "CoreMinimal.h"
#include "GPUScene.h"
#include "Renderers/MeshDrawCommandSubmission.h"
#include "CoreGlobals.h"

#include "RendererAPI.h"

#include "Renderers/MeshRenderPreparationCommon.h"
#include "Renderers/SurfaceMaterial.h"
#include "Materials/MaterialRenderProxy.h"
#include "Rendering/MeshBatch.h"
#include "Rendering/PrimitiveCustomData.h"
#include "Rendering/PrimitiveSceneTransform.h"
#include "RHIResources.h"
#include "UniformBuffer.h"
#include "Scene.h"
#include "SceneView.h"
#include "Renderers/GPUSceneRendering.h"
#include "Renderers/InstanceCullingDrawParams.h"
#include "Threading/Task.h"
#include "Renderers/ViewPreparationMath.h"

namespace Durin
{
	class FPrimitiveSceneInfo;
	class FRHICommandListImmediate;

	namespace RendererPrivate
	{
		struct FVisibleMeshDrawCommand;
		struct FParallelMeshDrawCommandPass;

		class FMeshVertexFactoryImplementation;
		struct FMeshFallbackPrimitiveUniform
		{
			FMatrix4f LocalToWorld{1.0f};
			FMatrix4f NormalToWorld{1.0f};
			FMatrix4f WorldToLocal{1.0f};
			FVector4f BoundsCenter{0.0f};
			FVector4f TransformParams{1.0f, 0.0f, 0.0f, 0.0f};
			FVector4f WorldOriginHigh{0.0f};
			FVector4f WorldOriginLow{0.0f};
			FPrimitiveCustomData CustomPrimitiveData;
			FPrimitiveCustomData CustomInstanceData;
		};
		static_assert(sizeof(FMeshFallbackPrimitiveUniform) == 512);
		static_assert(offsetof(FMeshFallbackPrimitiveUniform, CustomPrimitiveData) == 256);
		static_assert(offsetof(FMeshFallbackPrimitiveUniform, CustomInstanceData) == 384);

	}
	struct FMeshDrawDescription;
	struct FStaticMeshSectionFact
	{
		EGeometrySubmissionOutcome InputOutcome = EGeometrySubmissionOutcome::Submitted;
		bool bSupportsPreparation = false;
		bool bSupportsGBuffer = false;
	};
	struct FStaticMeshSectionFacts
	{
		std::shared_ptr<const FStaticMeshBatchInputs> Inputs;
		std::shared_ptr<const FVertexFactoryInputBinding> Binding;
		std::shared_ptr<const RendererPrivate::FMeshVertexFactoryImplementation> Factory;
		bool bFactoryCompatible = false;
		std::vector<FStaticMeshSectionFact> Elements;
	};
	struct FStaticMeshPassSections
	{
		std::shared_ptr<const FStaticMeshSectionFacts> Facts;
		std::vector<std::shared_ptr<const FMeshDrawDescription>> Commands;
	};
	struct FResolvedStaticMeshSection
	{
		std::shared_ptr<const FMeshDrawDescription> Command;
		uint32 MaterialUniformIndex = 0;
		bool bTemplateReused = false;
		bool bMaterialUnavailable = false;
	};
	class FMeshDrawDescriptionCache;
	struct FResolvedStaticMeshBatch
	{
		FPrimitiveSceneTransform Transform;
		std::vector<FResolvedStaticMeshSection> Elements;
		std::shared_ptr<const std::vector<FResolvedStaticMeshSection>> StaticElements;
		auto GetElement(size_t Index) const -> const FResolvedStaticMeshSection&
		{
			return StaticElements ? (*StaticElements)[Index] : Elements[Index];
		}
	};
	struct FMeshViewPreparationInput
	{
		FBox WorldBounds;
		std::optional<FMeshLODSelectionSnapshot> LODs;
		FStaticMeshBatchPublication::FRef StaticBatches;
	};
	struct FMeshViewPreparationFact
	{
		FProjectedScreenSizeResult Projected;
		std::optional<FPreparedMeshLODSelection> LOD;
	};
	struct FMeshViewPreparationFacts
	{
		std::vector<FMeshViewPreparationFact> Primitives;
		size_t TaskCount = 0;
	};
	RENDERER_API auto PrepareMeshViewFacts(std::vector<FMeshViewPreparationInput> Inputs,
		const FSceneView& View, bool bAllowTasks = true) -> FMeshViewPreparationFacts;

	// Command-local facts; retained providers share stable inputs while each view
	// independently selects LOD and captures instance state.
	struct FStaticMeshPreparationCache
	{
		FMeshDrawDescriptionCache* Commands = nullptr;
		struct FStaticBatchSnapshot
		{
			FMeshBatch Batch;
			FPrimitiveSceneTransform Transform;
		};
		struct FAdmittedBatch
		{
			std::shared_ptr<const FStaticBatchSnapshot> Snapshot;
			size_t Index;
		};
		struct FStaticBatchEntry
		{
			std::shared_ptr<const FStaticBatchSnapshot> Snapshot;
			ERasterMode RasterMode{};
			ESceneDepthConvention DepthConvention{};
			std::shared_ptr<const FStaticMeshSectionFacts> ShadowFacts;
			std::shared_ptr<const FResolvedStaticMeshBatch> ResolvedShadow;
		};
		struct FStaticPrimitive
		{
			FStaticMeshBatchPublication::FRef Publication;
			std::vector<size_t> AdmittedLODs;
		};
		// Scene/proxy inputs are stable for this single receiver/cascade submission.
		struct FStaticCollection
		{
			FStaticMeshBatchCaptureContext Capture;
			std::vector<FStaticBatchEntry> Batches;
			std::unordered_map<const FPrimitiveSceneInfo*, FStaticPrimitive> Primitives;
		} StaticCollection;
		// Frame-local uniform indices never enter the persistent Section tables.
		struct FResolvedSections
		{
			std::shared_ptr<const FStaticMeshPassSections> Owner;
			std::shared_ptr<const std::vector<FResolvedStaticMeshSection>> Elements;
		};
		std::unordered_map<const FStaticMeshPassSections*, FResolvedSections> ResolvedSections;
		FMeshBatchValidationCache Admission;
		std::unordered_multimap<uint64, uint32> MaterialIndices;
		std::map<std::pair<uint64, const FUniformExpressionCache*>, uint32> MaterialRecordIndices;
		std::vector<std::shared_ptr<const FUniformExpressionCache>> MaterialPublishers;
		std::vector<FMaterialRenderRepresentation> Materials;
		size_t MaterialBuilds = 0;

		RENDERER_API auto ResolveMaterial(FMaterialRenderData& Material) -> std::optional<uint32>;
	};

	// Larger values draw first. View depth is signed along the engine's +X
	// camera axis and is independent of the projection's device-depth convention.
	inline auto ComputeTranslucentSortDepth(
		const FSceneView& View, const FVector3& WorldCenter) -> double
	{
		const auto Policy = View.Settings.Mode.TranslucentSortPolicy;
		constexpr double ProjectionKindEpsilon = 1.0e-12;
		const bool bOrthographic = std::abs(View.ProjectionMatrix[0][3]) <= ProjectionKindEpsilon
			&& std::abs(View.ProjectionMatrix[1][3]) <= ProjectionKindEpsilon
			&& std::abs(View.ProjectionMatrix[2][3]) <= ProjectionKindEpsilon
			&& std::abs(View.ProjectionMatrix[3][3]) > ProjectionKindEpsilon;
		if (Policy == ETranslucentSortPolicy::ViewDepth
			|| (Policy == ETranslucentSortPolicy::Projection && bOrthographic))
		{
			return (View.ViewMatrix * FVector4(WorldCenter, 1.0)).x;
		}
		const FVector3 Offset = WorldCenter - View.ViewLocation;
		return Math::Dot(Offset, Offset);
	}

	// Stores one batch binding/transform shared by its prepared elements.
	struct FPreparedStaticMeshPrimitive
	{
		std::shared_ptr<const FGPUSceneDynamicPrimitiveData> DynamicPrimitiveData;
		FPrimitiveCustomData CustomPrimitiveData;
		FPrimitiveCustomData CustomInstanceData;
		FPrimitiveComponentId PrimitiveId = InvalidPrimitiveComponentId;
		uint64 BatchId = 0;
		EPrimitiveIdMode PrimitiveIdMode = EPrimitiveIdMode::PrimID_DynamicPrimitiveShaderData;
		FPersistentPrimitiveIndex PersistentPrimitiveIndex;
		FPrimitiveInstanceId InstanceSceneDataId;
		uint32 RequestedLODIndex = 0;
		uint32 SelectedLODIndex = 0;
		EVertexDeformationDomain VertexDomain = EVertexDeformationDomain::Local;
		std::shared_ptr<const FVertexFactoryInputBinding> CollectedBinding;
		std::shared_ptr<const FMeshGeometryRecord> GeometryRecord;
		FBox WorldBounds;
		FMatrix LocalToWorld{1.0};
		// Validated inverse-transpose in shader uniform storage order.
		FMatrix4f NormalToWorld{1.0f};
		FMatrix WorldToLocal{1.0};
		FVector3 BoundsCenter{0.0};
		bool bRetainedStaticMesh = false;
		FPrimitiveSceneInfoRef SceneInfo;
	};

	// View-independent interpretation of one published element and pass policy.
	struct FMeshDrawDescription
	{
		uint64 SectionIndex = 0;
		FGeometryDrawRange Geometry;
		FGeometryBufferView Vertices;
		FGeometryBufferView Indices;
		uint32 MaterialSlotDiagnostic = 0;
		bool bSupportsGBuffer = true;
		FMaterialRenderData Material;
		EMeshBasePass Pass = EMeshBasePass::Opaque;
		FEffectiveMeshPipelineKey PipelineKey;
		FMeshDrawSortKey SortKey;
		std::shared_ptr<const FMeshGeometryRecord> GeometryOwner;
	};

	// Scene-local retention. A prepared view owns any values it receives from the cache.
	class FMeshDrawDescriptionCache
	{
	public:
		using FKey = std::array<uint64, 5>;
		static constexpr size_t MaxEntries = 8192;
		static constexpr size_t MaxBytes = 8 * 1024 * 1024;
		static constexpr uint64 MaxIdleFrames = 120;
		RENDERER_API auto BeginSubmission(uint64 InFrame = GRenderFrameCounterRenderThread) -> void;
		RENDERER_API auto EndSubmission() -> void;
		RENDERER_API auto Find(const FKey& Key, const FMaterialRenderData& Material)
			-> std::shared_ptr<const FMeshDrawDescription>;
		RENDERER_API auto Store(const FKey& Key, std::shared_ptr<const FMeshDrawDescription> Command) -> void;
		RENDERER_API auto CaptureStaticFacts(const std::shared_ptr<const FStaticMeshBatchInputs>& Inputs, bool bShadow)
			-> std::shared_ptr<const FStaticMeshSectionFacts>;
		RENDERER_API auto FindStaticSections(const std::shared_ptr<const FStaticMeshBatchInputs>& Inputs, uint64 Policy)
			-> std::shared_ptr<const FStaticMeshPassSections>;
		RENDERER_API auto StoreStaticSections(uint64 Policy, std::shared_ptr<const FStaticMeshPassSections> Sections)
			-> void;
		auto Num() const -> size_t { return Entries.size(); }
		auto NumStaticFacts() const -> size_t { return StaticFacts.size(); }
		auto NumStaticSections() const -> size_t { return StaticSections.size(); }
		auto GetBudgetedBytes() const -> size_t { return BudgetedBytes; }
		auto GetEvictions() const -> size_t { return Evictions; }
		// Accounts cache-created objects, capacity of their owned vectors, and table values.
		// Publication, allocator metadata and RHI storage have independent owners.
		auto GetAccountedBytes() const -> size_t;
		RENDERER_API auto Reset() -> void;

	private:
		template<typename T>
		struct TEntry
		{
			std::shared_ptr<const T> Value;
			uint64 LastAccessFrame;
			size_t Charge;
		};
		std::map<FKey, TEntry<FMeshDrawDescription>> Entries;
		using FStaticKey = std::array<uint64, 2>;
		struct FStaticKeyHash
		{
			auto operator()(const FStaticKey& Key) const -> size_t
			{
				return static_cast<size_t>((Key[0] >> 4) ^ (Key[1] * 0x9e3779b97f4a7c15ull));
			}
		};
		std::unordered_map<FStaticKey, TEntry<FStaticMeshSectionFacts>, FStaticKeyHash> StaticFacts;
		std::unordered_map<FStaticKey, TEntry<FStaticMeshPassSections>, FStaticKeyHash> StaticSections;
		template<typename TTable, typename T>
		auto StoreEntry(TTable& Table, const typename TTable::key_type& Key, std::shared_ptr<const T> Value, size_t Weight) -> void;
		auto EvictIdle(uint64 IdleFrames) -> void;
		uint64 Frame = 0;
		uint64 LastAgeSweep = 0;
		std::optional<uint64> LastPressureSweep;
		size_t BudgetedBytes = 0;
		size_t Evictions = 0;
	};

	// Compact per-view references; stable planning state belongs to the description.
	struct FPreparedStaticMeshDraw
	{
		std::shared_ptr<const FMeshDrawDescription> Command;
		FVisibleMeshDrawSortKey SortKey;
		std::optional<std::array<float, 3>> RasterBias;
		uint32 ResolvedIndex = 0;
		uint32 PrimitiveIndex = 0;
		uint32 MaterialUniformIndex = UINT32_MAX;
		double TranslucentSortDepth = 0.0;
	};
	static_assert(sizeof(FPreparedStaticMeshDraw) <= 128, "Visible mesh draws must retain compact references, not stable payload copies.");

	struct FPreparedStaticMeshView
	{
		uint64 RenderFrame = GRenderFrameCounterRenderThread;
		struct FMaterialUniformGroup
		{
			uint32 RepresentativeDraw = 0;
		};
		std::vector<FMaterialUniformGroup> MaterialUniformGroups;
		std::vector<FPreparedStaticMeshPrimitive> Primitives;
		std::vector<FPreparedStaticMeshDraw> Opaque;
		std::vector<FPreparedStaticMeshDraw> Masked;
		std::vector<FPreparedStaticMeshDraw> Translucent;
		std::vector<size_t> RequestedLODHistogram;
		std::vector<size_t> SelectedLODHistogram;
		std::array<size_t, 6> SubmissionOutcomes{};
		bool bResourceFailure = false;
		// Retain component identity even when collection produced no usable draw.
		std::vector<FPrimitiveComponentId> ResourceFailurePrimitives;
		size_t VisibleCandidates = 0;
		size_t VisibleLocalCandidates = 0;
		size_t VisibleSplineCandidates = 0;
		size_t PreparedLocalPrimitives = 0;
		size_t PreparedSplinePrimitives = 0;
		size_t RejectedSplinePrimitives = 0;
		size_t PreparedSplineSections = 0;
		size_t PreparedSplineTriangles = 0;
		size_t RetainedSplineDeformationBytes = 0;
		size_t AcceptedSplineDynamicUpdates = 0;
		size_t RejectedPrimitives = 0;
		size_t ProjectedSizeFallbacks = 0;
		size_t ResourceFallbacks = 0;
		size_t SelectedSections = 0;
		size_t SelectedTriangles = 0;
		size_t OpaqueSections = 0;
		size_t MaskedSections = 0;
		size_t TranslucentSections = 0;
		size_t OpaqueTriangles = 0;
		size_t MaskedTriangles = 0;
		size_t TranslucentTriangles = 0;
		size_t OpaqueStateGroups = 0;
		size_t MaskedStateGroups = 0;
		size_t OpaqueInputStateGroups = 0;
		size_t MaskedInputStateGroups = 0;
		size_t PipelineTransitions = 0;
		size_t MaterialTransitions = 0;
		size_t VertexFactoryTransitions = 0;
		size_t GeometryTransitions = 0;
		uint64 SortingNanoseconds = 0;
		size_t PreparationTaskCount = 0;
		uint64 PreparationJoinNanoseconds = 0;
		size_t SharedPrimitiveFactBuilds = 0;
		size_t SelectedLODFactBuilds = 0;
		size_t SharedSectionFactBuilds = 0;
		size_t DynamicGeometryInputValidations = 0;
		size_t PublishedGeometryElements = 0;
		size_t DrawDescriptionBuilds = 0;
		size_t DrawDescriptionReuses = 0;

		auto GetDraw(uint32 Index) const -> const FPreparedStaticMeshDraw&
		{
			if (Index < Opaque.size()) return Opaque[Index];
			Index -= static_cast<uint32>(Opaque.size());
			if (Index < Masked.size()) return Masked[Index];
			return Translucent[Index - Masked.size()];
		}

		auto GetNumSections() const -> size_t
		{
			return Opaque.size() + Masked.size() + Translucent.size();
		}

		auto GetPrimitive(const FPreparedStaticMeshDraw& Draw) const
			-> const FPreparedStaticMeshPrimitive*
		{
			return Draw.PrimitiveIndex < Primitives.size() ? &Primitives[Draw.PrimitiveIndex] : nullptr;
		}
	};

	struct FStaticMeshRenderObservations
	{
		bool bRecordedOnWorker = false;
		size_t PrimitiveUniformUploads = 0;
		size_t PrimitiveUniformReuses = 0;
		size_t PersistentCommandHits = 0;
		size_t PersistentCommandMisses = 0;
		size_t MaterialUniformUploads = 0;
		size_t SurfaceMaterialPreparations = 0;
		size_t PreparedSurfaceBindingBatches = 0;
		size_t SurfaceBindingBatchReuses = 0;
		size_t ResourcePreparationAttemptedDraws = 0;
		size_t ResourcePreparationSuccessfulDraws = 0;
		size_t ResourcePreparationRejectedDraws = 0;
		size_t AttemptedDraws = 0;
		size_t SuccessfulDraws = 0;
		size_t NativeDrawSubmissions = 0;
		size_t RejectedDraws = 0;
		size_t GBufferAttemptedDraws = 0;
		size_t GBufferSuccessfulDraws = 0;
		size_t GBufferRejectedDraws = 0;
		size_t GBufferSkippedDraws = 0;
		size_t GBufferLocalAttemptedDraws = 0;
		size_t GBufferLocalSuccessfulDraws = 0;
		size_t GBufferLocalDirectSubmissions = 0;
		size_t GBufferIndirectSubmissions = 0;
		size_t GBufferLocalRejectedDraws = 0;
		size_t GBufferLocalSkippedDraws = 0;
		size_t GBufferSplineAttemptedDraws = 0;
		size_t GBufferSplineSuccessfulDraws = 0;
		size_t GBufferSplineRejectedDraws = 0;
		size_t GBufferSplineSkippedDraws = 0;
	};

	// Owns fallible bindings without mutating logical draws.
	struct FSkyLightingGeneration;
	struct FResolvedStaticMeshView
	{
		FBufferRHIRef DynamicInstancingInstanceIds;
		std::shared_ptr<const FGPUSceneBufferSet> GPUSceneBuffers;
		std::array<size_t, 2> GBufferSkippedSources{};
		std::vector<uint8> GBufferSourceFamilies;
		RendererPrivate::FMeshDrawCommandPassStats* GBufferPassStats = nullptr;
		RENDERER_API auto GetInstanceCullingDrawParams(FRDGBuilder& Graph, FInstanceCullingDrawParams Parameters = {}) const -> FInstanceCullingDrawParams;
		FGPUScene* GPUScene = nullptr;
		std::shared_ptr<FGPUSceneDynamicContext> GPUSceneDynamicContext;
		std::vector<FGPUScenePrimitiveData> GPUScenePrimitives;
		RendererPrivate::FCachedMeshDrawCommandStorage* CachedDrawLists = nullptr;
		std::shared_ptr<const FSkyLightingGeneration> Environment;
		struct FMaterialUniform
		{
			RendererPrivate::FResolvedSurfaceMaterial Surface;
			FRHIUniformBufferRange Uniform;
			std::vector<FRHIUniformBufferRange> CollectionUniforms;
		};
		std::vector<FResolvedMeshDrawRecord> Draws;
		// Selected before uniform/surface preparation; stable bindings survive into recording.
		std::vector<bool> PersistentCommandsReused;
		// A retained command can reuse its geometry/pipelines while Collection ranges change.
		// Only unsupported vertex factories allocate the legacy uniform contract.
        std::unordered_map<uint32, FRHIUniformBufferRange> FallbackPrimitiveUniforms;
        auto GetFallbackPrimitiveUniform(uint32 Index) const -> FRHIUniformBufferRange
        {
            const auto Found = FallbackPrimitiveUniforms.find(Index);
            return Found == FallbackPrimitiveUniforms.end() ? FRHIUniformBufferRange{} : Found->second;
        }
		std::map<RendererPrivate::ESurfaceMaterialPass, FRHIUniformBufferRange> ViewUniforms;
		// Typed surface resources can be shared by compatible mesh passes.
		std::vector<std::map<RendererPrivate::ESurfaceMaterialPass, std::optional<FMaterialUniform>>> MaterialUniforms;
		std::shared_ptr<RendererPrivate::FDynamicMeshDrawCommandStorage> DrawCommandStorage;
		std::map<RendererPrivate::FMeshPassKey, std::shared_ptr<RendererPrivate::FParallelMeshDrawCommandPass>> MeshDrawCommandPasses;
		RENDERER_API auto GetMeshDrawCommandPass(RendererPrivate::FMeshPassKey Pass) -> RendererPrivate::FParallelMeshDrawCommandPass&;
		RENDERER_API auto GetMeshDrawCommandPass(RendererPrivate::FMeshPassKey Pass) const -> const RendererPrivate::FParallelMeshDrawCommandPass&;
		RENDERER_API auto GetVisibleMeshDrawCommand(const FPreparedStaticMeshDraw& Draw, RendererPrivate::FMeshPassKey Pass) const
			-> const RendererPrivate::FVisibleMeshDrawCommand*;
		FRHITexture* DirectionalShadowTexture = nullptr;
		FRHISampler* DirectionalShadowSampler = nullptr;
		FStaticMeshRenderObservations Observations;

		auto IsReady(const FPreparedStaticMeshDraw& Draw) const -> bool
		{
			return Draw.ResolvedIndex < Draws.size()
				&& Draws[Draw.ResolvedIndex].bReady;
		}
		auto GetMaterialBinding(const FPreparedStaticMeshDraw& Draw) const
			-> const FMaterialRenderBinding*
		{
			return Draw.ResolvedIndex < Draws.size()
				&& Draws[Draw.ResolvedIndex].MaterialBinding
				? &*Draws[Draw.ResolvedIndex].MaterialBinding : nullptr;
		}
	};

	// Finalize one collection's draw lists in place, preserving stable resolved indices.
	RENDERER_API auto CollectGPUScenePrimitives(const FPreparedStaticMeshView& Prepared, const std::shared_ptr<FGPUSceneDynamicContext>& Context)
		-> std::vector<FGPUScenePrimitiveData>;
	RENDERER_API auto AdmitStaticMeshDraws(FPreparedStaticMeshView& View, const FResolvedStaticMeshView& Resources) -> size_t;

	// Owns collected geometry/material values after all scene/proxy reads finish.
	// Dynamic provider bindings retain their existing resource-retirement contract.
	struct FCollectedStaticMeshView
	{
		uint64 RenderFrame = GRenderFrameCounterRenderThread;
		using FElementFacts = FStaticMeshSectionFact;
		struct FBatch
		{
			std::variant<FStaticMeshPreparationCache::FAdmittedBatch, FMeshBatch> Source;
			// Views can request different LODs and select the same admitted fallback.
			uint32 RequestedLOD = 0;
			auto GetBatch() const -> const FMeshBatch&
			{
				if (auto* Dynamic = std::get_if<FMeshBatch>(&Source)) return *Dynamic;
				return std::get<FStaticMeshPreparationCache::FAdmittedBatch>(Source).Snapshot->Batch;
			}
			std::shared_ptr<const FGPUSceneDynamicPrimitiveData> DynamicPrimitiveData;
			std::shared_ptr<const FVertexFactoryInputBinding> InputBinding;
			bool bFactoryCompatible = false;
			bool bReverseSplineWinding = false;
			bool bSplineChangesOrientation = false;
			std::vector<FElementFacts> Elements;
			std::shared_ptr<const FStaticMeshSectionFacts> StaticFacts;
			auto GetElementFacts(size_t Index) const -> const FElementFacts&
			{
				return StaticFacts ? StaticFacts->Elements[Index] : Elements[Index];
			}
		};
		struct FPrimitive
		{
			FPrimitiveComponentId Id = InvalidPrimitiveComponentId;
			FPrimitiveSceneInfoRef SceneInfo;
				FPersistentPrimitiveIndex PersistentPrimitiveIndex;
			ERHIFeatureLevel FeatureLevel = ERHIFeatureLevel::SM5;
			bool bPresent = false;
			bool bSplineMesh = false;
			bool bRetainedStaticMesh = false;
			// Empty when projection can run on the immutable preparation snapshot after admission.
			std::optional<EProjectedScreenSizeStatus> ProjectionStatus = EProjectedScreenSizeStatus::Valid;
			FBox WorldBounds;
			decltype(FPreparedStaticMeshView::SubmissionOutcomes) Outcomes{};
			std::vector<FBatch> Batches;
		};
		FSceneView View;
		ERasterMode RasterMode = ERasterMode::Solid;
		ERenderPreparationMode Mode = ERenderPreparationMode::Full;
		std::vector<FPrimitive> Primitives;
	};

	RENDERER_API auto CollectStaticMeshView_RenderThread(
		const FRHICommandListImmediate& CommandList,
		std::span<const FPrimitiveSceneInfo* const> SceneInfos,
		const FSceneView& View, ERasterMode RasterMode,
		ERenderPreparationMode Mode = ERenderPreparationMode::Full,
		FStaticMeshPreparationCache* SharedCache = nullptr)
		-> std::shared_ptr<const FCollectedStaticMeshView>;

	// Every shared-cache decision is resolved before this immutable owner is
	// published. Logical consumers need neither a command list nor a cache.
	struct FStaticMeshPreparationInputs
	{
		using FElement = FResolvedStaticMeshSection;
		using FBatch = FResolvedStaticMeshBatch;
		std::shared_ptr<const FCollectedStaticMeshView> Collected;
		struct FPrimitive
		{
			std::vector<FBatch> Batches;
			bool bSourceAvailable = true;
			std::shared_ptr<const FBatch> StaticShadowBatch;
			auto GetBatch(size_t Index) const -> const FBatch&
			{
				return StaticShadowBatch ? *StaticShadowBatch : Batches[Index];
			}
		};
		std::vector<FPrimitive> Primitives;
		size_t MaterialCount = 0;
	};

	RENDERER_API auto ResolveCollectedStaticMeshView_RenderThread(
		const FRHICommandListImmediate& CommandList,
		std::shared_ptr<const FCollectedStaticMeshView> Collected,
		FStaticMeshPreparationCache* SharedCache = nullptr) -> std::shared_ptr<const FStaticMeshPreparationInputs>;

	struct FStaticMeshPreparationChunk
	{
		std::shared_ptr<const FStaticMeshPreparationInputs> Inputs;
		size_t FirstPrimitive = 0;
		size_t PrimitiveCount = 0;
		FPreparedStaticMeshView Output;
	};

	enum class EStaticMeshPreparationMergeError : uint8
	{
		EmptyChunks,
		MismatchedInputs,
		InvalidRange,
		IncompleteCoverage,
		TooManyPrimitives,
	};

	RENDERER_API auto PrepareStaticMeshInputChunk(
		std::shared_ptr<const FStaticMeshPreparationInputs> Inputs,
		size_t FirstPrimitive, size_t PrimitiveCount) -> FStaticMeshPreparationChunk;

	RENDERER_API auto MergeStaticMeshPreparationChunks(std::vector<FStaticMeshPreparationChunk> Chunks)
		-> std::expected<FPreparedStaticMeshView, EStaticMeshPreparationMergeError>;

	enum class EStaticMeshPreparationPolicy : uint8 { Auto, Inline, Tasks };
	struct FStaticMeshPreparationError
	{
		size_t ChunkIndex = 0;
		ETaskState State = ETaskState::Failed;
		std::optional<EStaticMeshPreparationMergeError> MergeError;
	};
	struct FStaticMeshPreparationWork
	{
		std::optional<FPreparedStaticMeshView> InlineOutput;
		std::vector<Tasks::FTask> Workers;
		Tasks::TTask<std::expected<FPreparedStaticMeshView, FStaticMeshPreparationError>> Completion;
		std::function<bool()> IsCancellationRequested;
		bool bCanceled = false;
		FStaticMeshPreparationWork() = default;
		FStaticMeshPreparationWork(FStaticMeshPreparationWork&&) noexcept = default;
		FStaticMeshPreparationWork(const FStaticMeshPreparationWork&) = delete;
		auto operator=(FStaticMeshPreparationWork&&) -> FStaticMeshPreparationWork& = delete;
		RENDERER_API ~FStaticMeshPreparationWork();
	};
	RENDERER_API auto StartStaticMeshPreparation(std::shared_ptr<const FStaticMeshPreparationInputs> Inputs,
		EStaticMeshPreparationPolicy Policy = EStaticMeshPreparationPolicy::Auto,
		std::function<bool()> IsCancellationRequested = {}) -> FStaticMeshPreparationWork;
	RENDERER_API auto FinishStaticMeshPreparation(FStaticMeshPreparationWork Work)
		-> std::expected<FPreparedStaticMeshView, FStaticMeshPreparationError>;

	RENDERER_API auto PrepareStaticMeshInputs(const FStaticMeshPreparationInputs& Resolved) -> FPreparedStaticMeshView;

	RENDERER_API auto BeginStaticMeshView_RenderThread(
		const FRHICommandListImmediate& CommandList,
		std::span<const FPrimitiveSceneInfo* const> SceneInfos,
		const FSceneView& View, ERasterMode RasterMode, ERenderPreparationMode Mode,
		FStaticMeshPreparationCache* SharedCache = nullptr) -> FStaticMeshPreparationWork;
	RENDERER_API auto FinishStaticMeshView(FStaticMeshPreparationWork Work) -> FPreparedStaticMeshView;

	RENDERER_API auto PrepareStaticMeshView_RenderThread(
		const FRHICommandListImmediate& CommandList,
		std::span<const FPrimitiveSceneInfo* const> SceneInfos,
		const FSceneView& View,
		ERasterMode RasterMode,
		ERenderPreparationMode Mode = ERenderPreparationMode::Full,
		FStaticMeshPreparationCache* SharedCache = nullptr
	) -> FPreparedStaticMeshView;
} // namespace Durin

namespace Durin::RendererPrivate
{
	RENDERER_API auto BuildFallbackPrimitiveUniform(const FPreparedStaticMeshPrimitive& Primitive) -> FMeshFallbackPrimitiveUniform;
	// Explicit uniform transport for factories without GPUScene capability.
	auto PrepareFallbackPrimitiveUniform(FRHICommandListImmediate& Commands, const FPreparedStaticMeshPrimitive& Primitive) -> FRHIUniformBufferRange;
	// Returns no declaration when the admitted draw's geometry is invalid.
	auto ResolveMeshVertexDeclaration(const FPreparedStaticMeshPrimitive& Primitive,
		const FPreparedStaticMeshDraw& Draw) -> FVertexDeclarationRHIRef;

	template<typename TPreparedView>
	auto GetBasePassBucket(
		TPreparedView& PreparedView,
		EMeshBasePass Pass
	) -> decltype(auto)
	{
		return Pass == EMeshBasePass::Opaque ?
				   (PreparedView.Opaque) :
			   Pass == EMeshBasePass::Masked ?
				   (PreparedView.Masked) :
				   (PreparedView.Translucent);
	}

	template<typename TPreparedView, typename TFunction>
	auto ForEachBasePassBucket(
		TPreparedView& PreparedView,
		TFunction&& Function
	) -> void
	{
		std::invoke(Function, PreparedView.Opaque, EMeshBasePass::Opaque);
		std::invoke(Function, PreparedView.Masked, EMeshBasePass::Masked);
		std::invoke(
			Function,
			PreparedView.Translucent,
			EMeshBasePass::Translucent
		);
	}

	template<typename TPreparedView, typename TFunction>
	auto ForEachShadowBucket(
		TPreparedView& PreparedView,
		TFunction&& Function
	) -> void
	{
		std::invoke(Function, PreparedView.Opaque);
		std::invoke(Function, PreparedView.Masked);
	}

}
