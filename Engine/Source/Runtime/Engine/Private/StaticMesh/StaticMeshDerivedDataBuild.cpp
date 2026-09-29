#include "StaticMesh/StaticMeshBuild.h"

#include "Asset/AssetBuildServicePrivate.h"
#include "StaticMeshBuildFunction.h"
#include "StaticMeshSharedOutput.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshDerivedDataKey.h"

namespace Durin
{
	auto FormatStaticMeshRenderBuildError(const FStaticMeshRenderBuildError& Error) -> std::string
	{
		std::string_view Reason;
		switch (Error.Code)
		{
		case EStaticMeshRenderBuildError::None: return {};
		case EStaticMeshRenderBuildError::MissingGeometry: Reason = "requires decoded geometry."; break;
		case EStaticMeshRenderBuildError::VertexLimit: Reason = "exceeds the uint32 vertex limit."; break;
		case EStaticMeshRenderBuildError::TriangleList: Reason = "index count is not a triangle list."; break;
		case EStaticMeshRenderBuildError::NonFinitePosition: Reason = "contains a non-finite position."; break;
		case EStaticMeshRenderBuildError::IndexRange: Reason = "contains an out-of-range index."; break;
		case EStaticMeshRenderBuildError::WorkingSet: Reason = "predicted working set exceeds its reservation."; break;
		case EStaticMeshRenderBuildError::DuplicateMaterial: Reason = "has a duplicate imported source material index."; break;
		case EStaticMeshRenderBuildError::RenderLimits: Reason = "exceeds uint32 render-data limits."; break;
		case EStaticMeshRenderBuildError::MissingMaterial: Reason = "references a missing source material."; break;
		case EStaticMeshRenderBuildError::EmptyGeometry: Reason = "source has no renderable geometry."; break;
		case EStaticMeshRenderBuildError::Bounds: Reason = "source has invalid bounds."; break;
		case EStaticMeshRenderBuildError::Cancelled: Reason = "build was cancelled."; break;
		}
		return std::format("StaticMesh render build: {} (mesh '{}', section '{}', index {}, actual {}, expected {}).",
			Reason,
			Error.MeshName, Error.SectionName, Error.Index, Error.Actual, Error.Expected);
	}

#if DURIN_WITH_EDITOR
	namespace
	{
		auto RestoreRuntimeMetadata(
			std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
			FStaticMeshRenderData& RenderData) -> std::expected<void, std::string>
		{
			if (RenderData.MaterialSlots.size() != MaterialSlots.size())
			{
				return std::unexpected(std::format("Cached material slot count {} does not match {}.", RenderData.MaterialSlots.size(), MaterialSlots.size()));
			}
			for (size_t SlotIndex = 0; SlotIndex < MaterialSlots.size(); ++SlotIndex)
			{
				RenderData.MaterialSlots[SlotIndex].Name =
					MaterialSlots[SlotIndex].Name.ToString();
				RenderData.MaterialSlots[SlotIndex].SourceMaterialIndex =
					MaterialSlots[SlotIndex].SourceMaterialIndex;
			}
			for (size_t LODIndex = 0; LODIndex < RenderData.LODResources.size(); ++LODIndex)
				for (size_t SectionIndex = 0;
					SectionIndex < RenderData.LODResources[LODIndex].Sections.size();
					++SectionIndex)
					RenderData.LODResources[LODIndex].Sections[SectionIndex].Name =
						std::format("LOD{}_Section{}", LODIndex, SectionIndex);
			return {};
		}

	}

#endif

	auto BuildStaticMeshRenderData(FStaticMeshBuildRequest Request,
		const FAssetBuildTaskContext& Control) -> std::expected<std::unique_ptr<FStaticMeshRenderData>, FStaticMeshBuildFailure>
	{
		if (Control.IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Render));
		if (!Request.Source.IsValid() || !std::isfinite(Request.Reconciliation.NormalizedSize)
			|| Request.Reconciliation.NormalizedSize <= 0)
			return std::unexpected(FStaticMeshBuildFailure{"StaticMesh render source or normalization is invalid.", EStaticMeshBuildStage::Source});
		FAssetBuildMemoryEstimate SourceMemory{Control.MaximumWorkingSetBytes};
		if (!SourceMemory.Add(Request.Source.GetGeometryBulk().GetPayloadSize(), 8)
			|| !SourceMemory.Add(Request.Source.GetMeshCount(), sizeof(FStaticMeshImportedMesh))
			|| !SourceMemory.Add(Request.Source.GetMaterialSlotCount(), 32768))
			return std::unexpected(FStaticMeshBuildFailure{"StaticMesh decoded source exceeds its reservation.", EStaticMeshBuildStage::Validation});
		if (Request.Reconciliation.MaterialSlots.size() > MaximumMeshMaterialSlots)
			return std::unexpected(FStaticMeshBuildFailure{"StaticMesh material-slot input exceeds the slot limit.", EStaticMeshBuildStage::Render});
		std::unordered_set<FName> SlotNames;
		std::unordered_set<uint32> SourceIndices;
		for (const auto& Slot : Request.Reconciliation.MaterialSlots)
		{
			if (Slot.Name.IsNone() || Slot.SourceName.size() > 4096
				|| !SlotNames.insert(Slot.Name).second || !SourceIndices.insert(Slot.SourceMaterialIndex).second)
				return std::unexpected(FStaticMeshBuildFailure{"StaticMesh requires bounded, uniquely named material slots with unambiguous source indices.", EStaticMeshBuildStage::Render});
		}
		bool bCancelled = false;
		const auto IsCancelled = [&] {
			bCancelled = bCancelled || Control.IsCancelled();
			return bCancelled;
		};
		if (IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Render));
#if !DURIN_WITH_EDITOR
		return std::unexpected(FStaticMeshBuildFailure{"StaticMesh build orchestration is unavailable outside editor builds.", EStaticMeshBuildStage::Render});
#else
		auto Definition = MakeStaticMeshSessionDefinition(uint32(Request.Reconciliation.MaterialSlots.size()));
		if (!Definition) return std::unexpected(FStaticMeshBuildFailure{
			"StaticMesh build definition is invalid.", EStaticMeshBuildStage::Source});
		auto Inputs = StaticMeshPrivate::MakeRenderInputResolver(Request);
		// The resolver retains canonical bulk, not the request's optional decoded residency.
		Request.Source.ReleaseGeometry();
		DerivedData::FBuildRequestOptions Options;
		Options.Policy.StoreOnBuild = Request.bPersistDerivedData;
		Options.Policy.MaximumWorkingSetBytes = Control.MaximumWorkingSetBytes;
		Options.Policy.InputLimits.MaximumTotalBytes = MaximumStaticMeshSourceBytes + 64ull * 1024 * 1024;
		Options.Policy.OutputLimits.MaximumTotalBytes = MaximumStaticMeshPayloadBytes;
		const uint64 MaximumBytes = std::min(MaximumStaticMeshPayloadBytes, Control.MaximumWorkingSetBytes / 16);
		Options.Policy.PersistenceLimits.MaximumTotalBytes = MaximumBytes;
		Options.Policy.MaximumEncodedBytes = MaximumBytes;
		Options.Cancellation = DerivedData::FBuildCancellation(IsCancelled);
		auto Built = AssetBuildPrivate::Build(std::move(*Definition), std::move(Inputs), std::move(Options));
		if (!Built) return std::unexpected(FStaticMeshBuildFailure{
			"StaticMesh build service is unavailable.", EStaticMeshBuildStage::Render});
		auto& Completion = *Built;
		if (IsCancelled() || Completion.GetStatus() == DerivedData::EStatus::Canceled)
			return std::unexpected(FStaticMeshBuildFailure::Cancelled());
		if (Completion.GetStatus() == DerivedData::EStatus::Error)
		{
			std::string Description = "StaticMesh derived-data build failed.";
			if (const auto* Output = Completion.GetOutput(); Output && !Output->GetMessages().empty()) Description = Output->GetMessages().back().Text;
			else if (const auto* Output = Completion.GetOutput(); Output && !Output->GetLogs().empty()) Description = Output->GetLogs().back().Text;
			return std::unexpected(FStaticMeshBuildFailure{std::move(Description), EStaticMeshBuildStage::Render});
		}
		auto Product = StaticMeshPrivate::AssembleSharedOutput(*Completion.GetOutput(), IsCancelled);
		if (IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Validation));
		if (!Product) return std::unexpected(FStaticMeshBuildFailure{std::move(Product.error()), EStaticMeshBuildStage::Validation});
		if (auto Metadata = RestoreRuntimeMetadata(Request.Reconciliation.MaterialSlots, **Product); !Metadata)
			return std::unexpected(FStaticMeshBuildFailure{std::move(Metadata.error()), EStaticMeshBuildStage::Validation});
		if (auto Valid = FinalizeStaticMeshRenderData(**Product, Control); !Valid) return std::unexpected(std::move(Valid.error()));
		return std::move(*Product);
#endif
	}

}
