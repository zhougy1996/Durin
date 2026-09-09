#pragma once

#include "GeometrySubmission.h"
#include "Materials/MaterialRenderProxy.h"
#include "Math/Operations.h"
#include "PrimitiveComponentId.h"
#include "VertexFactory.h"

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
	struct FMeshCollectionContext
	{
		FPrimitiveComponentId PrimitiveId = InvalidPrimitiveComponentId;
		FMatrix LocalToWorld{1.0};
		FBox WorldBounds;
		float NormalizedScreenSize = 1.0f;
		EMeshCollectionProjectionStatus ProjectionStatus = EMeshCollectionProjectionStatus::InvalidView;
		bool bForceLOD0 = false;
		EMeshCollectionPurpose Purpose = EMeshCollectionPurpose::Receiver;
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

	// Value snapshots and retained bindings survive provider updates. Borrowed
	// asset data inside a concrete binding still obeys its retirement fence.
	struct FMeshBatch
	{
		FPrimitiveComponentId PrimitiveId = InvalidPrimitiveComponentId;
		uint64 BatchId = 0;
		FMatrix LocalToWorld{1.0};
		FBox WorldBounds;
		FXxHash64 FactoryKey;
		FXxHash64 LayoutKey;
		std::shared_ptr<const FVertexFactoryBinding> Binding;
		bool bReceiver = true;
		bool bShadowCaster = true;
		std::vector<FMeshBatchElement> Elements;
		// Optional provider diagnostics; execution never dereferences an asset LOD.
		uint32 RequestedLOD = 0;
		uint32 SelectedLOD = 0;
		uint32 LODCount = 1;
		uint64 AcceptedDynamicUpdates = 0;
		uint64 RetainedDeformationBytes = 0;
	};

	// Owns admitted snapshots. A rejected batch cannot partially enter the frame.
	class FMeshBatchCollector
	{
	public:
		explicit FMeshBatchCollector(EMeshCollectionPurpose InPurpose)
			: Purpose(InPurpose) {}

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
			if (Batch.Elements.empty()) return EGeometrySubmissionOutcome::Empty;
			if (Batch.PrimitiveId == InvalidPrimitiveComponentId
				|| !Math::IsFinite(Batch.LocalToWorld)
				|| !Batch.WorldBounds.bIsValid
				|| !Math::IsFinite(Batch.WorldBounds.Min)
				|| !Math::IsFinite(Batch.WorldBounds.Max)
				|| Batch.WorldBounds.Min.x > Batch.WorldBounds.Max.x
				|| Batch.WorldBounds.Min.y > Batch.WorldBounds.Max.y
				|| Batch.WorldBounds.Min.z > Batch.WorldBounds.Max.z
				|| Batch.LODCount == 0 || Batch.RequestedLOD >= Batch.LODCount
				|| Batch.SelectedLOD >= Batch.LODCount
				|| !Batch.Binding || Batch.FactoryKey.IsZero() || Batch.LayoutKey.IsZero()
				|| Batch.FactoryKey != Batch.Binding->GetFactoryKey()
				|| Batch.LayoutKey != Batch.Binding->GetLayoutKey())
				return EGeometrySubmissionOutcome::InvalidSubmission;
			FMatrix WorldToLocal;
			if (!Math::TryInverse(Batch.LocalToWorld, WorldToLocal))
				return EGeometrySubmissionOutcome::InvalidSubmission;
			if (std::ranges::any_of(Batches, [&](const FMeshBatch& Existing) {
				return Existing.PrimitiveId == Batch.PrimitiveId && Existing.BatchId == Batch.BatchId;
			})) return EGeometrySubmissionOutcome::InvalidSubmission;
			bool bHasDraws = false;
			for (size_t Index = 0; Index < Batch.Elements.size(); ++Index)
			{
				const auto& Element = Batch.Elements[Index];
				for (size_t Previous = 0; Previous < Index; ++Previous)
					if (Batch.Elements[Previous].ElementId == Element.ElementId)
						return EGeometrySubmissionOutcome::InvalidSubmission;
				const auto Outcome = Element.Draw.Validate(Element.Vertices.Range, Element.Indices.Range);
				if (Outcome == EGeometrySubmissionOutcome::Empty) continue;
				if (Outcome != EGeometrySubmissionOutcome::Submitted) return Outcome;
				if (Element.Draw.bIndexed && Element.Indices.Range.ByteOffset > std::numeric_limits<uint32>::max())
					return EGeometrySubmissionOutcome::Unsupported;
				if (!Element.Vertices.IsValid()
					|| (Element.Draw.bIndexed && !Element.Indices.IsValid()))
					return EGeometrySubmissionOutcome::ResourceFailure;
				if (Element.Draw.bIndexed && Element.Indices.Buffer->GetStride() != Element.Indices.Range.ElementBytes)
					return EGeometrySubmissionOutcome::InvalidSubmission;
				if (!EnumHasAnyFlags(Element.Vertices.Buffer->GetUsage(), EBufferUsageFlags::VertexBuffer)
					|| (Element.Draw.bIndexed && !EnumHasAnyFlags(
						Element.Indices.Buffer->GetUsage(), EBufferUsageFlags::IndexBuffer)))
					return EGeometrySubmissionOutcome::InvalidSubmission;
				for (const auto& Stream : Element.InstanceStreams)
				{
					if (!Stream.Range.Contains(Element.Draw.FirstInstance, Element.Draw.InstanceCount))
						return EGeometrySubmissionOutcome::InvalidSubmission;
					if (!Stream.IsValid()) return EGeometrySubmissionOutcome::ResourceFailure;
					if (!EnumHasAnyFlags(Stream.Buffer->GetUsage(), EBufferUsageFlags::VertexBuffer))
						return EGeometrySubmissionOutcome::InvalidSubmission;
				}
				bHasDraws = true;
			}
			if (!bHasDraws) return EGeometrySubmissionOutcome::Empty;
			Batches.push_back(std::move(Batch));
			return EGeometrySubmissionOutcome::Submitted;
		}

		std::array<size_t, 6> OutcomeCounts{};
		EMeshCollectionPurpose Purpose;
		std::vector<FMeshBatch> Batches;
	};
}
