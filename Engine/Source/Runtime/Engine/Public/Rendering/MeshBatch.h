#pragma once

#include "CoreMinimal.h"

#include "GeometrySubmission.h"
#include "Materials/MaterialRenderProxy.h"
#include "Math/Operations.h"
#include "Rendering/PrimitiveComponentId.h"
#include "Rendering/PrimitiveCustomData.h"
#include "Rendering/MeshBatchValidationCache.h"
#include "VertexFactory.h"
#include "Rendering/MeshGeometryRecord.h"
#include "Rendering/StaticMeshBatchPublication.h"

namespace Durin
{
	// Renderer supplies scalar view facts; providers never depend on scene views.
	enum class EMeshCollectionProjectionStatus : uint8
	{
		Valid,
		NearPlaneOrCameraCrossing,
		InvalidBounds,
		InvalidView,
	};

	enum class EMeshCollectionPurpose : uint8
	{
		Receiver,
		Shadow,
	};

	// Render-thread input whose lifetime is restricted to the collection call.
	struct FPreparedMeshLODSelection
	{
		uint32 Requested = 0;
		uint32 Selected = std::numeric_limits<uint32>::max();
	};
	struct FMeshCollectionContext
	{
		uint64 BatchId = 0;
		FPrimitiveCustomData CustomPrimitiveData;
		FPrimitiveCustomData CustomInstanceData;
		FPrimitiveComponentId PrimitiveId = InvalidPrimitiveComponentId;
		FMatrix LocalToWorld{1.0};
		FBox WorldBounds;
		float NormalizedScreenSize = 1.0f;
		EMeshCollectionProjectionStatus ProjectionStatus = EMeshCollectionProjectionStatus::InvalidView;
		bool bForceLOD0 = false;
		EMeshCollectionPurpose Purpose = EMeshCollectionPurpose::Receiver;
		std::optional<FPreparedMeshLODSelection> PreparedLOD;
	};

	// One materialized element with explicit direct draw and stream ranges.
	struct FMeshBatchElement
	{
		uint64 ElementId = 0;
		FGeometryDrawRange Draw;
		FGeometryBufferView Vertices;
		FGeometryBufferView Indices;
		std::vector<FGeometryBufferView> InstanceStreams;
		FMaterialRenderData Material;
		FBox LocalBounds;
		uint32 MaterialSlotDiagnostic = 0;
	};

	struct FMeshBatchElementView
	{
		uint64 ElementId;
		const FGeometryDrawRange& Draw;
		const FGeometryBufferView& Vertices;
		const FGeometryBufferView& Indices;
		std::span<const FGeometryBufferView> InstanceStreams;
		const FMaterialRenderData& Material;
		const FBox& LocalBounds;
		uint32 MaterialSlotDiagnostic;
	};

	// Value snapshots and retained bindings survive provider updates. Borrowed
	// asset data inside a concrete binding still obeys its retirement fence.
	struct FMeshBatch
	{
		FPrimitiveCustomData CustomPrimitiveData;
		FPrimitiveCustomData CustomInstanceData;
		FPrimitiveComponentId PrimitiveId = InvalidPrimitiveComponentId;
		uint64 BatchId = 0;
		FMatrix LocalToWorld{1.0};
		FBox WorldBounds;
		FXxHash64 FactoryKey;
		FXxHash64 LayoutKey;
		std::shared_ptr<const FVertexFactoryBinding> Binding;
		FMeshGeometryRecord::FRef GeometryRecord;
		bool bReceiver = true;
		bool bShadowCaster = true;
		std::vector<FMeshBatchElement> Elements;
		// Exactly one material per published element. Dynamic batches use Elements.
		std::vector<FMaterialRenderData> PublishedMaterials;
		// Ordinary StaticMesh uses an immutable owner instead of rebuilding the
		// per-section material array for every primitive/view/cascade.
		std::shared_ptr<const FStaticMeshBatchInputs> StaticInputs;
		// Optional provider diagnostics; execution never dereferences an asset LOD.
		uint32 RequestedLOD = 0;
		uint32 SelectedLOD = 0;
		uint32 LODCount = 1;
		uint64 AcceptedDynamicUpdates = 0;
		uint64 RetainedDeformationBytes = 0;

		auto GetNumElements() const -> size_t
		{
			return GeometryRecord ? GeometryRecord->GetElements().size() : Elements.size();
		}
		auto GetPublishedMaterials() const -> std::span<const FMaterialRenderData>
		{
			return StaticInputs ? std::span<const FMaterialRenderData>(StaticInputs->Materials)
				: std::span<const FMaterialRenderData>(PublishedMaterials);
		}
		auto GetElement(size_t Index) const -> FMeshBatchElementView
		{
			if (GeometryRecord)
			{
				const auto& Element = GeometryRecord->GetElements()[Index];
				return {Element.ElementId, Element.Draw, Element.Vertices, Element.Indices,
					Element.InstanceStreams, GetPublishedMaterials()[Index], Element.LocalBounds, Element.MaterialSlotDiagnostic};
			}
			const auto& Element = Elements[Index];
			return {Element.ElementId, Element.Draw, Element.Vertices, Element.Indices,
				Element.InstanceStreams, Element.Material, Element.LocalBounds, Element.MaterialSlotDiagnostic};
		}
	};

	// Owns admitted snapshots. A rejected batch cannot partially enter the frame.
	class FMeshBatchCollector
	{
	public:
		explicit FMeshBatchCollector(EMeshCollectionPurpose InPurpose,
			FMeshBatchValidationCache* InValidationCache = nullptr)
			: Purpose(InPurpose), ValidationCache(InValidationCache) {}

		auto Add(FMeshBatch Batch) -> EGeometrySubmissionOutcome
		{
			const auto Outcome = Admit(std::move(Batch));
			RecordOutcome(Outcome);
			return Outcome;
		}

		auto RecordOutcome(EGeometrySubmissionOutcome Outcome) -> void
		{
			++OutcomeCounts[static_cast<size_t>(Outcome)];
		}
		auto GetOutcomeCount(EGeometrySubmissionOutcome Outcome) const -> size_t
		{
			return OutcomeCounts[static_cast<size_t>(Outcome)];
		}
		auto GetBatches() const -> std::span<const FMeshBatch> { return Batches; }
		// Transfers checked snapshots to the frame builder without copying materials.
		auto TakeBatches() && -> std::vector<FMeshBatch> { return std::move(Batches); }

	private:
		auto Admit(FMeshBatch Batch) -> EGeometrySubmissionOutcome
		{
			if ((Purpose == EMeshCollectionPurpose::Receiver && !Batch.bReceiver)
				|| (Purpose == EMeshCollectionPurpose::Shadow && !Batch.bShadowCaster))
				return EGeometrySubmissionOutcome::Excluded;
			if ((Batch.StaticInputs && (!Batch.GeometryRecord || !Batch.PublishedMaterials.empty()
					|| Batch.StaticInputs->Geometry != Batch.GeometryRecord))
				|| (Batch.GeometryRecord && (!Batch.Elements.empty()
				|| Batch.GetPublishedMaterials().size() != Batch.GetNumElements()
				|| Batch.Binding != Batch.GeometryRecord->GetBinding()))
				|| (!Batch.GeometryRecord && !Batch.PublishedMaterials.empty()))
				return EGeometrySubmissionOutcome::InvalidSubmission;
			if (Batch.GetNumElements() == 0) return EGeometrySubmissionOutcome::Empty;
			if (ValidationCache
					? !ValidationCache->ValidateValues(Batch.PrimitiveId, Batch.BatchId, Batch.WorldBounds,
						  Batch.CustomPrimitiveData, Batch.CustomInstanceData)
					: (!Batch.CustomPrimitiveData.IsValid() || !Batch.CustomInstanceData.IsValid()
						  || !Batch.WorldBounds.bIsValid || !Math::IsFinite(Batch.WorldBounds.Min)
						  || !Math::IsFinite(Batch.WorldBounds.Max) || Batch.WorldBounds.Min.x > Batch.WorldBounds.Max.x
						  || Batch.WorldBounds.Min.y > Batch.WorldBounds.Max.y
						  || Batch.WorldBounds.Min.z > Batch.WorldBounds.Max.z))
				return EGeometrySubmissionOutcome::InvalidSubmission;
			if (Batch.PrimitiveId == InvalidPrimitiveComponentId
				|| Batch.LODCount == 0 || Batch.RequestedLOD >= Batch.LODCount
				|| Batch.SelectedLOD >= Batch.LODCount
				|| !Batch.Binding || Batch.FactoryKey.IsZero() || Batch.LayoutKey.IsZero()
				|| Batch.FactoryKey != Batch.Binding->GetFactoryKey()
				|| Batch.LayoutKey != Batch.Binding->GetLayoutKey())
				return EGeometrySubmissionOutcome::InvalidSubmission;
			FMatrix WorldToLocal;
			if (ValidationCache
					? !ValidationCache->ResolveTransform(Batch.PrimitiveId, Batch.BatchId, Batch.LocalToWorld)
						  .bAdmissible
					: (!Math::IsFinite(Batch.LocalToWorld) || !Math::TryInverse(Batch.LocalToWorld, WorldToLocal)))
				return EGeometrySubmissionOutcome::InvalidSubmission;
			if (std::ranges::any_of(Batches, [&](const FMeshBatch& Existing) {
				return Existing.PrimitiveId == Batch.PrimitiveId && Existing.BatchId == Batch.BatchId;
			})) return EGeometrySubmissionOutcome::InvalidSubmission;
			if (Batch.GeometryRecord)
			{
				Batches.push_back(std::move(Batch));
				return EGeometrySubmissionOutcome::Submitted;
			}
			bool bHasDraws = false;
			for (size_t Index = 0; Index < Batch.Elements.size(); ++Index)
			{
				const auto& Element = Batch.Elements[Index];
				for (size_t Previous = 0; Previous < Index; ++Previous)
					if (Batch.Elements[Previous].ElementId == Element.ElementId)
						return EGeometrySubmissionOutcome::InvalidSubmission;
				const auto Outcome = ValidateMeshGeometryElement(Element.Draw, Element.Vertices,
					Element.Indices, Element.InstanceStreams);
				if (Outcome == EGeometrySubmissionOutcome::Empty) continue;
				if (Outcome != EGeometrySubmissionOutcome::Submitted) return Outcome;
				bHasDraws = true;
			}
			if (!bHasDraws) return EGeometrySubmissionOutcome::Empty;
			Batches.push_back(std::move(Batch));
			return EGeometrySubmissionOutcome::Submitted;
		}

		std::array<size_t, 6> OutcomeCounts{};
		EMeshCollectionPurpose Purpose;
		FMeshBatchValidationCache* ValidationCache = nullptr;
		std::vector<FMeshBatch> Batches;
	};
}
