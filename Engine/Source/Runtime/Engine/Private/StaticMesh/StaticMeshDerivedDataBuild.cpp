#include "StaticMesh/StaticMeshBuild.h"

#include "Asset/AssetDerivedDataCache.h"
#include "Serialization/Archive.h"
#include "Serialization/BinaryFormat.h"
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

		// Transfers build storage without copying vertex streams or initializing RHI resources.
		auto AssembleRenderData(FStaticMeshRenderBuildProduct& Product,
			std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
			const std::function<bool()>& ShouldCancel) -> std::unique_ptr<FStaticMeshRenderData>
		{
			auto RenderData = std::make_unique<FStaticMeshRenderData>();
			RenderData->LocalBounds = Product.LocalBounds;
			RenderData->MaterialSlots.reserve(MaterialSlots.size());
			for (const auto& Slot : MaterialSlots)
			{
				if (ShouldCancel()) return {};
				RenderData->MaterialSlots.push_back({Slot.Name.ToString(), Slot.SourceMaterialIndex});
			}
			RenderData->LODResources.reserve(Product.LODs.size());
			for (auto& Source : Product.LODs)
			{
				if (ShouldCancel()) return {};
				auto& LOD = RenderData->LODResources.emplace_back();
				auto& Buffers = LOD.VertexBuffers;
				Buffers.PositionVertexBuffer.GetMutablePositions() = std::move(Source.Positions);
				Buffers.StaticMeshVertexBuffer.TangentsVertexBuffer.GetMutableNormals() = std::move(Source.Normals);
				Buffers.StaticMeshVertexBuffer.TangentsVertexBuffer.GetMutableTangents() = std::move(Source.Tangents);
				Buffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.GetMutableTexCoords() = std::move(Source.TexCoords);
				Buffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.SetNumTexCoords(Source.NumTexCoords);
				Buffers.ColorVertexBuffer.GetMutableColors() = std::move(Source.Colors);
				LOD.IndexBuffer.GetMutableIndices() = std::move(Source.Indices);
				LOD.Sections = std::move(Source.Sections);
				LOD.LocalBounds = Source.LocalBounds;
				LOD.ScreenSize = Source.ScreenSize;
				LOD.NumTexCoords = Source.NumTexCoords;
				LOD.bHasColorVertexData = Source.bHasColorVertexData;
			}
			return RenderData;
		}

		auto ArchiveCodecFailure(const FArchive& Ar) -> std::expected<void, std::string>
		{
			const auto* Failure = Ar.GetFailure();
			return std::unexpected(std::format("Payload archive failed at byte {} (Archive code {}, path '{}'): {}",
				Ar.Tell(), Failure ? static_cast<int>(Failure->Code) : -1,
				Failure ? Failure->Path : std::string{}, Ar.GetError()));
		}

		auto EncodeRenderData(const FStaticMeshRenderData& RenderData, FByteBuffer& OutBytes,
			const std::function<bool()>& ShouldCancel) -> std::expected<void, std::string>
		{
			FStaticMeshPayloadData Payload;
			if (const auto Built = MakeStaticMeshPayloadData(RenderData, Payload, ShouldCancel); !Built)
				return std::unexpected(FormatStaticMeshPayloadError(Built.error()));
			OutBytes.clear();
			FCanonicalMemoryWriter Ar(OutBytes, EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
			Payload.Serialize(Ar, ShouldCancel);
			if (!Ar.IsError()) return {};
			const auto Failure = ArchiveCodecFailure(Ar);
			OutBytes.clear();
			return Failure;
		}

		auto DecodeRenderData(FByteView Bytes, std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
			std::unique_ptr<FStaticMeshRenderData>& OutRenderData,
			const std::function<bool()>& ShouldCancel) -> std::expected<void, std::string>
		{
			FStaticMeshPayloadData Payload;
			FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
			Payload.Serialize(Ar, ShouldCancel);
			if (Ar.IsError() || !RequireArchiveEnd(Ar)) return ArchiveCodecFailure(Ar);
			if (const auto Built = MakeStaticMeshRenderData(Payload, OutRenderData, ShouldCancel); !Built)
				return std::unexpected(FormatStaticMeshPayloadError(Built.error()));
			return RestoreRuntimeMetadata(MaterialSlots, *OutRenderData);
		}


	}

#endif
	auto BuildStaticMeshRenderData(FStaticMeshBuildRequest Request,
		const FAssetBuildTaskContext& Control, std::vector<FAssetBuildCacheWarning>* OutCacheWarnings)
		-> std::expected<std::unique_ptr<FStaticMeshRenderData>, FStaticMeshBuildFailure>
	{
		return BuildStaticMeshRenderDataInSession(FStaticMeshBuildSession::Acquire(),
			std::move(Request), Control, OutCacheWarnings);
	}

	auto BuildStaticMeshRenderDataInSession(
		FStaticMeshBuildSession Session, FStaticMeshBuildRequest Request,
		const FAssetBuildTaskContext& Control, std::vector<FAssetBuildCacheWarning>* OutCacheWarnings) -> std::expected<std::unique_ptr<FStaticMeshRenderData>, FStaticMeshBuildFailure>
	{
		if (OutCacheWarnings) OutCacheWarnings->clear();
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
		if (!Session) return std::unexpected(FStaticMeshBuildFailure{
			"The StaticMesh build module is unavailable; workers require a retained build session.", EStaticMeshBuildStage::Render});
		auto Build = [&]() -> std::expected<std::unique_ptr<FStaticMeshRenderData>, FStaticMeshBuildFailure> {
			const FStaticMeshBuilderDescriptor Descriptor = Session.GetModule().GetDescriptor();
			if (!Descriptor.IsValid())
			{
				return std::unexpected(FStaticMeshBuildFailure{std::format("Invalid StaticMesh builder '{}' (render version {}).",
						Descriptor.ProducerIdentity, Descriptor.RenderBuilderVersion), EStaticMeshBuildStage::Render});
			}
			FStaticMeshBuildKeyInput KeyInput{
				.SourceHash = Request.Source.GetIdentity(),
				.ReconciliationHash = BuildStaticMeshReconciliationHash(
					Request.Reconciliation.MaterialSlots,
					Request.Reconciliation.NormalizedSize),
				.BuilderVersion = Descriptor.RenderBuilderVersion,
				.TargetPlatform = EAssetPayloadTargetPlatform::Win64};
			auto KeyResult = BuildStaticMeshDerivedDataKey(KeyInput);
			if (!KeyResult)
			{
				return std::unexpected(FStaticMeshBuildFailure{FormatStaticMeshBuildKeyError(KeyResult.error()), EStaticMeshBuildStage::Render});
			}
			FCacheKeyProxy Key = *KeyResult;
			AssetDerivedDataCache::FOperationDiagnostic LoadDiagnostic;
			FByteBuffer Bytes;
			std::optional<std::string> CacheDecodeCause;
			if (AssetDerivedDataCache::Load(
				Key, std::min(MaximumStaticMeshPayloadBytes, Control.MaximumWorkingSetBytes / 16),
				Bytes, LoadDiagnostic) == AssetDerivedDataCache::ELoadResult::Hit)
			{
				std::unique_ptr<FStaticMeshRenderData> RenderData;
				const auto Decoded = DecodeRenderData(Bytes, Request.Reconciliation.MaterialSlots, RenderData, IsCancelled);
				if (Decoded)
				{
					return RenderData;
				}
				CacheDecodeCause = Decoded.error();
			}
			if (IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Render));
			auto Decoded = Request.Source.AcquireGeometry(IsCancelled);
			if (!Decoded)
				return std::unexpected(Decoded.error().Code == EStaticMeshSourceError::Cancelled
					? FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Source, FormatStaticMeshSourceError(Decoded.error()))
					: FStaticMeshBuildFailure{FormatStaticMeshSourceError(Decoded.error()), EStaticMeshBuildStage::Source});
			if (IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Render));
			std::vector<FStaticMeshBuildMaterialSlot> BuildSlots;
			for (const auto& Slot : Request.Reconciliation.MaterialSlots)
				BuildSlots.push_back({Slot.Name, Slot.SourceName, Slot.SourceMaterialIndex});
			auto BuildOutcome = Session.GetModule().BuildRender({
				.Geometry = std::move(*Decoded),
				.MaterialSlots = BuildSlots,
				.NormalizedSize = Request.Reconciliation.NormalizedSize}, Control);
			if (!BuildOutcome)
			{
				return std::unexpected(BuildOutcome.error().Code == EStaticMeshRenderBuildError::Cancelled
					? FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Render, FormatStaticMeshRenderBuildError(BuildOutcome.error()))
					: FStaticMeshBuildFailure{FormatStaticMeshRenderBuildError(BuildOutcome.error()), EStaticMeshBuildStage::Render});
			}
			if (IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Render));
			auto& BuildProduct = *BuildOutcome;
			auto RenderData = AssembleRenderData(BuildProduct, Request.Reconciliation.MaterialSlots, IsCancelled);
			if (!RenderData) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Render));
			if (const auto Encoded = EncodeRenderData(*RenderData, Bytes, IsCancelled); !Encoded)
			{
				if (IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Render));
				return std::unexpected(FStaticMeshBuildFailure{Encoded.error(), EStaticMeshBuildStage::Render});
			}
			AssetDerivedDataCache::FOperationDiagnostic StoreDiagnostic;
			if (IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Render));
			if (Request.bPersistDerivedData)
				AssetDerivedDataCache::Store(Key, Bytes,
					MaximumStaticMeshPayloadBytes, StoreDiagnostic);
			if (OutCacheWarnings) *OutCacheWarnings = AssetDerivedDataCache::CollectBuildWarnings(LoadDiagnostic, StoreDiagnostic, CacheDecodeCause);
			return RenderData;
		};
		auto Outcome = Build();
		if (IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Render));
		if (Outcome)
		{
			if (!*Outcome) return std::unexpected(FStaticMeshBuildFailure{"StaticMesh builder returned no render data.", EStaticMeshBuildStage::Validation});
			if (const auto Finalized = FinalizeStaticMeshRenderData(**Outcome, Control); !Finalized)
				return std::unexpected(Finalized.error());
		}
		return Outcome;
#endif
	}

}
