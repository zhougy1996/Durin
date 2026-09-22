#pragma once

#include "RendererAPI.h"

#include "Renderers/MeshRenderPreparationCommon.h"
#include "Renderers/SurfaceMaterial.h"
#include "Materials/MaterialRenderProxy.h"
#include "Rendering/MeshBatch.h"
#include "RHIResources.h"
#include "Scene.h"
#include "SceneView.h"
#include "Threading/TaskComposition.h"
#include "Renderers/ViewPreparationMath.h"

#include <vector>
#include <deque>
#include <map>
#include <unordered_map>

namespace Durin
{
	class FRHICommandListImmediate;

	class FStaticMeshDrawCommandCache;
	struct FMeshViewPreparationInput
	{
		FBox WorldBounds;
		std::optional<FMeshLODSelectionSnapshot> LODs;
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

	// Command-local facts only. Providers still collect independently for each view/LOD.
	struct FStaticMeshPreparationCache
	{
		FStaticMeshDrawCommandCache* Commands = nullptr;
		struct FTransform
		{
			FMatrix LocalToWorld{1.0};
			FMatrix4f NormalToWorld{1.0f};
			double Determinant = 0.0;
			bool bValid = false;
		};
		std::map<std::pair<uint64, uint64>, FTransform> Transforms;
		std::unordered_multimap<uint64, uint32> MaterialIndices;
		std::unordered_map<uint64, uint32> MaterialRecordIndices;
		std::vector<FMaterialRenderRepresentation> Materials;
		size_t TransformBuilds = 0;
		size_t MaterialBuilds = 0;

		RENDERER_API auto ResolveTransform(FPrimitiveComponentId PrimitiveId,
			uint64 BatchId, const FMatrix& LocalToWorld) -> const FTransform&;
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
		FPrimitiveComponentId PrimitiveId = InvalidPrimitiveComponentId;
		uint64 BatchId = 0;
		uint32 RequestedLODIndex = 0;
		uint32 SelectedLODIndex = 0;
		EVertexDeformationDomain VertexDomain = EVertexDeformationDomain::Local;
		std::shared_ptr<const FVertexFactoryInputBinding> CollectedBinding;
		std::shared_ptr<const FMeshGeometryRecord> GeometryRecord;
		FMatrix LocalToWorld{1.0};
		// Validated inverse-transpose in shader uniform storage order.
		FMatrix4f NormalToWorld{1.0f};
	};

	// View-independent interpretation of one published element and pass policy.
	struct FStaticMeshDrawCommandTemplate
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

	// Cache retention is bounded to the templates touched by one submission.
	// Old frames independently retain their immutable selected templates.
	class FStaticMeshDrawCommandCache
	{
	public:
		using FKey = std::array<uint64, 4>;
		RENDERER_API auto BeginSubmission() -> void;
		RENDERER_API auto EndSubmission() -> void;
		RENDERER_API auto Find(const FKey& Key, const FMaterialRenderData& Material)
			-> std::shared_ptr<const FStaticMeshDrawCommandTemplate>;
		RENDERER_API auto Store(const FKey& Key, std::shared_ptr<const FStaticMeshDrawCommandTemplate> Command) -> void;
		auto Num() const -> size_t { return Entries.size(); }
		auto Reset() -> void { Entries.clear(); }
	private:
		struct FEntry { std::shared_ptr<const FStaticMeshDrawCommandTemplate> Command; bool bUsed = false; };
		std::map<FKey, FEntry> Entries;
	};

	// Compact per-view references; all stable state belongs to the template.
	struct FPreparedStaticMeshDraw
	{
		std::shared_ptr<const FStaticMeshDrawCommandTemplate> Command;
		FVisibleMeshDrawSortKey SortKey;
		std::optional<std::array<float, 3>> RasterBias;
		uint32 ResolvedIndex = 0;
		uint32 PrimitiveIndex = 0;
		uint32 MaterialUniformIndex = UINT32_MAX;
		FVector3 SortCenter{0.0};
		double TranslucentSortDepth = 0.0;
	};
	static_assert(sizeof(FPreparedStaticMeshDraw) <= 128, "Visible mesh draws must retain compact references, not stable payload copies.");

	struct FPreparedStaticMeshView
	{
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
		size_t CommandTemplateBuilds = 0;
		size_t CommandTemplateReuses = 0;

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
		size_t MaterialUniformUploads = 0;
		size_t PreparedSurfaceBindingBatches = 0;
		size_t SurfaceBindingBatchReuses = 0;
		size_t ResourcePreparationAttemptedDraws = 0;
		size_t ResourcePreparationSuccessfulDraws = 0;
		size_t ResourcePreparationRejectedDraws = 0;
		size_t AttemptedDraws = 0;
		size_t SuccessfulDraws = 0;
		size_t RejectedDraws = 0;
		size_t GBufferAttemptedDraws = 0;
		size_t GBufferSuccessfulDraws = 0;
		size_t GBufferRejectedDraws = 0;
		size_t GBufferSkippedDraws = 0;
		size_t GBufferLocalAttemptedDraws = 0;
		size_t GBufferLocalSuccessfulDraws = 0;
		size_t GBufferLocalRejectedDraws = 0;
		size_t GBufferLocalSkippedDraws = 0;
		size_t GBufferSplineAttemptedDraws = 0;
		size_t GBufferSplineSuccessfulDraws = 0;
		size_t GBufferSplineRejectedDraws = 0;
		size_t GBufferSplineSkippedDraws = 0;
	};

	// Owns fallible bindings without mutating logical draws.
	struct FResolvedStaticMeshView
	{
		struct FMaterialUniform
		{
			RendererPrivate::FResolvedSurfaceMaterial Surface;
			FRHIUniformBufferRange Uniform;
		};
		std::vector<FResolvedMeshDrawRecord> Draws;
		// Dense primitive/material indices avoid per-draw pointer-keyed lookups.
		std::vector<FRHIUniformBufferRange> PrimitiveUniforms;
		std::array<FRHIUniformBufferRange, 3> ViewUniforms;
		// Forward, GBuffer, and masked-shadow slots are prepared before recording.
		std::vector<std::array<std::optional<FMaterialUniform>, 3>> MaterialUniforms;
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

	// Owns collected geometry/material values after all scene/proxy reads finish.
	// Dynamic provider bindings retain their existing resource-retirement contract.
	struct FCollectedStaticMeshView
	{
		struct FElementFacts
		{
			EGeometrySubmissionOutcome InputOutcome = EGeometrySubmissionOutcome::Submitted;
			bool bSupportsPreparation = false;
			bool bSupportsGBuffer = false;
		};
		struct FBatch
		{
			FMeshBatch Batch;
			std::shared_ptr<const FVertexFactoryInputBinding> InputBinding;
			bool bFactoryCompatible = false;
			std::vector<FElementFacts> Elements;
		};
		struct FPrimitive
		{
			FPrimitiveComponentId Id = InvalidPrimitiveComponentId;
			bool bPresent = false;
			bool bSplineMesh = false;
			bool bProjectedSizeFallback = false;
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
		ERenderPreparationMode Mode = ERenderPreparationMode::Full)
		-> std::shared_ptr<const FCollectedStaticMeshView>;

	// Every shared-cache decision is resolved before this immutable owner is
	// published. Logical consumers need neither a command list nor a cache.
	struct FStaticMeshPreparationInputs
	{
		struct FElement
		{
			std::shared_ptr<const FStaticMeshDrawCommandTemplate> Command;
			uint32 MaterialUniformIndex = 0;
			bool bTemplateReused = false;
		};
		struct FBatch
		{
			FStaticMeshPreparationCache::FTransform Transform;
			std::vector<FElement> Elements;
		};
		std::shared_ptr<const FCollectedStaticMeshView> Collected;
		std::vector<std::vector<FBatch>> Primitives;
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
	struct FStaticMeshPreparationWork
	{
		struct FSlot
		{
			size_t Index;
			Tasks::TTask<FStaticMeshPreparationChunk> Task;
		};
		std::shared_ptr<const FStaticMeshPreparationInputs> Inputs;
		std::optional<FPreparedStaticMeshView> InlineOutput;
		std::deque<FSlot> Active;
		FTaskCancellationToken Cancellation;
		bool bCanceled = false;
		size_t NextPrimitive = 0;
		size_t LaunchedChunks = 0;
		FStaticMeshPreparationWork() = default;
		FStaticMeshPreparationWork(FStaticMeshPreparationWork&&) noexcept = default;
		FStaticMeshPreparationWork(const FStaticMeshPreparationWork&) = delete;
		auto operator=(FStaticMeshPreparationWork&&) -> FStaticMeshPreparationWork& = delete;
		RENDERER_API ~FStaticMeshPreparationWork();
	};
	struct FStaticMeshPreparationError
	{
		size_t ChunkIndex = 0;
		ETaskState State = ETaskState::Failed;
		std::optional<EStaticMeshPreparationMergeError> MergeError;
	};
	RENDERER_API auto StartStaticMeshPreparation(std::shared_ptr<const FStaticMeshPreparationInputs> Inputs,
		EStaticMeshPreparationPolicy Policy = EStaticMeshPreparationPolicy::Auto,
		FTaskCancellationToken Cancellation = {}) -> FStaticMeshPreparationWork;
	RENDERER_API auto FinishStaticMeshPreparation(FStaticMeshPreparationWork Work)
		-> std::expected<FPreparedStaticMeshView, FStaticMeshPreparationError>;

	RENDERER_API auto PrepareStaticMeshInputs(const FStaticMeshPreparationInputs& Resolved) -> FPreparedStaticMeshView;

	RENDERER_API auto PrepareCollectedStaticMeshView_RenderThread(
		const FRHICommandListImmediate& CommandList,
		std::shared_ptr<const FCollectedStaticMeshView> Collected,
		FStaticMeshPreparationCache* SharedCache = nullptr) -> FPreparedStaticMeshView;
	RENDERER_API auto StartCollectedStaticMeshView_RenderThread(
		const FRHICommandListImmediate& CommandList,
		std::shared_ptr<const FCollectedStaticMeshView> Collected,
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
