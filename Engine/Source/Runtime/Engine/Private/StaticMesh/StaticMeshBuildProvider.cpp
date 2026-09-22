#include "StaticMesh/StaticMeshBuilder.h"

#include "Asset/AssetDerivedDataCache.h"
#include "Serialization/Archive.h"
#include "Serialization/BinaryFormat.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshDerivedDataKey.h"

namespace Durin
{
	auto FormatStaticMeshRecipeError(const FStaticMeshRecipeError& Error) -> std::string
	{
		std::string_view Reason;
		switch (Error.Code)
		{
		case EStaticMeshRecipeError::None: return {};
		case EStaticMeshRecipeError::MissingGeometry: Reason = "requires decoded geometry."; break;
		case EStaticMeshRecipeError::VertexLimit: Reason = "exceeds the uint32 vertex limit."; break;
		case EStaticMeshRecipeError::TriangleList: Reason = "index count is not a triangle list."; break;
		case EStaticMeshRecipeError::NonFinitePosition: Reason = "contains a non-finite position."; break;
		case EStaticMeshRecipeError::IndexRange: Reason = "contains an out-of-range index."; break;
		case EStaticMeshRecipeError::WorkingSet: Reason = "predicted working set exceeds its reservation."; break;
		case EStaticMeshRecipeError::DuplicateMaterial: Reason = "has a duplicate imported source material index."; break;
		case EStaticMeshRecipeError::RenderLimits: Reason = "exceeds uint32 render-data limits."; break;
		case EStaticMeshRecipeError::MissingMaterial: Reason = "references a missing source material."; break;
		case EStaticMeshRecipeError::EmptyGeometry: Reason = "source has no renderable geometry."; break;
		case EStaticMeshRecipeError::Bounds: Reason = "source has invalid bounds."; break;
		case EStaticMeshRecipeError::Cancelled: Reason = "recipe was cancelled."; break;
		}
		return std::format("StaticMesh render recipe: {} (mesh '{}', section '{}', index {}, actual {}, expected {}).",
			Reason,
			Error.MeshName, Error.SectionName, Error.Index, Error.Actual, Error.Expected);
	}

	auto FormatStaticMeshCacheCodecError(const FStaticMeshCacheCodecError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EStaticMeshCacheCodecError::None: return {};
		case EStaticMeshCacheCodecError::RenderPayload:
			return Error.RenderCause ? FormatStaticMeshPayloadError(*Error.RenderCause) : "StaticMesh render payload conversion failed.";
		case EStaticMeshCacheCodecError::Archive:
			return std::format("StaticMesh cache payload operation {} failed at byte {} (Archive code {}, path '{}').",
				static_cast<int>(Error.Operation), Error.Actual, Error.ArchiveCode ? static_cast<int>(*Error.ArchiveCode) : -1, Error.ArchivePath);
		case EStaticMeshCacheCodecError::MaterialSlots:
			return std::format("Cached StaticMesh material slot count {} does not match asset metadata count {}.", Error.Actual, Error.Expected);

		}
		return {};
	}

#if DURIN_WITH_EDITOR
	namespace
	{
		auto RestoreRuntimeMetadata(
			std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
			FStaticMeshRenderData& RenderData) -> std::expected<void, FStaticMeshCacheCodecError>
		{
			if (RenderData.MaterialSlots.size() != MaterialSlots.size())
			{
				return std::unexpected(FStaticMeshCacheCodecError{.Code = EStaticMeshCacheCodecError::MaterialSlots, .Operation = EStaticMeshCacheCodecOperation::DecodeRender,
					.Actual = RenderData.MaterialSlots.size(), .Expected = MaterialSlots.size()});
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

		// Transfers recipe storage without copying vertex streams or initializing RHI resources.
		auto AssembleRenderData(FStaticMeshRecipeBuildProduct& Product,
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

		auto ArchiveCodecFailure(const FArchive& Ar, EStaticMeshCacheCodecOperation Operation) -> std::expected<void, FStaticMeshCacheCodecError>
		{
			FStaticMeshCacheCodecError Error{.Code = EStaticMeshCacheCodecError::Archive, .Operation = Operation, .Actual = Ar.Tell()};
			if (const auto* Failure = Ar.GetFailure())
			{
				Error.ArchiveCode = Failure->Code;
				Error.ArchivePath = Failure->Path;
			}
			return std::unexpected(std::move(Error));
		}

		auto EncodeRenderData(const FStaticMeshRenderData& RenderData, FByteBuffer& OutBytes,
			const std::function<bool()>& ShouldCancel) -> std::expected<void, FStaticMeshCacheCodecError>
		{
			FStaticMeshPayloadData Payload;
			if (const auto Built = MakeStaticMeshPayloadData(RenderData, Payload, ShouldCancel); !Built)
				return std::unexpected(FStaticMeshCacheCodecError{.Code = EStaticMeshCacheCodecError::RenderPayload, .Operation = EStaticMeshCacheCodecOperation::EncodeRender, .RenderCause = Built.error()});
			OutBytes.clear();
			FCanonicalMemoryWriter Ar(OutBytes, EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
			Payload.Serialize(Ar, ShouldCancel);
			if (!Ar.IsError()) return {};
			const auto Failure = ArchiveCodecFailure(Ar, EStaticMeshCacheCodecOperation::EncodeRender);
			OutBytes.clear();
			return Failure;
		}

		auto DecodeRenderData(FByteView Bytes, std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
			std::unique_ptr<FStaticMeshRenderData>& OutRenderData,
			const std::function<bool()>& ShouldCancel) -> std::expected<void, FStaticMeshCacheCodecError>
		{
			FStaticMeshPayloadData Payload;
			FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
			Payload.Serialize(Ar, ShouldCancel);
			if (Ar.IsError() || !RequireArchiveEnd(Ar)) return ArchiveCodecFailure(Ar, EStaticMeshCacheCodecOperation::DecodeRender);
			if (const auto Built = MakeStaticMeshRenderData(Payload, OutRenderData, ShouldCancel); !Built)
				return std::unexpected(FStaticMeshCacheCodecError{.Code = EStaticMeshCacheCodecError::RenderPayload, .Operation = EStaticMeshCacheCodecOperation::DecodeRender, .RenderCause = Built.error()});
			return RestoreRuntimeMetadata(MaterialSlots, *OutRenderData);
		}

		auto CollectCacheErrors(
			const AssetDerivedDataCache::FOperationDiagnostic& Read,
			const AssetDerivedDataCache::FOperationDiagnostic& Write,
			const std::optional<FStaticMeshCacheCodecError>& Decode) -> std::vector<FStaticMeshCacheError>
		{
			std::vector<FStaticMeshCacheError> Errors;
			if (Read.Code != EAssetCacheError::None)
				Errors.emplace_back( EStaticMeshCacheOperation::Read, FormatAssetCacheDiagnostic(Read));
			if (Decode)
				Errors.emplace_back( EStaticMeshCacheOperation::Decode, FormatStaticMeshCacheCodecError(*Decode));
			if (Write.Code != EAssetCacheError::None)
				Errors.emplace_back( EStaticMeshCacheOperation::Write, FormatAssetCacheDiagnostic(Write));
			return Errors;
		}

	}

#endif
	auto FStaticMeshBuilder::Build(
		FStaticMeshBuildRequest Request,
		const FAssetBuildTaskContext& Control, std::vector<FStaticMeshCacheError>* OutCacheErrors, uint64 ExpectedProviderRegistration) -> std::expected<std::unique_ptr<FStaticMeshRenderData>, FStaticMeshBuildFailure>
	{
		if (OutCacheErrors) OutCacheErrors->clear();
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
		auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<
			IStaticMeshBuildProvider>([&](IStaticMeshBuildProvider& Provider) -> std::expected<std::unique_ptr<FStaticMeshRenderData>, FStaticMeshBuildFailure> {
			const FStaticMeshBuildProviderDescriptor Descriptor = Provider.GetDescriptor();
			if (!Descriptor.IsValid())
			{
				return std::unexpected(FStaticMeshBuildFailure{std::format("Invalid StaticMesh provider '{}' (render version {}).",
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
			std::optional<FStaticMeshCacheCodecError> CacheDecodeCause;
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
			std::vector<FStaticMeshRecipeMaterialSlot> RecipeSlots;
			for (const auto& Slot : Request.Reconciliation.MaterialSlots)
				RecipeSlots.push_back({Slot.Name, Slot.SourceName, Slot.SourceMaterialIndex});
			auto RecipeOutcome = Provider.BuildRender({
				.Geometry = std::move(*Decoded),
				.MaterialSlots = RecipeSlots,
				.NormalizedSize = Request.Reconciliation.NormalizedSize}, Control);
			if (!RecipeOutcome)
			{
				return std::unexpected(RecipeOutcome.error().Code == EStaticMeshRecipeError::Cancelled
					? FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Render, FormatStaticMeshRecipeError(RecipeOutcome.error()))
					: FStaticMeshBuildFailure{FormatStaticMeshRecipeError(RecipeOutcome.error()), EStaticMeshBuildStage::Render});
			}
			if (IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Render));
			auto& RecipeProduct = *RecipeOutcome;
			auto RenderData = AssembleRenderData(RecipeProduct, Request.Reconciliation.MaterialSlots, IsCancelled);
			if (!RenderData) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Render));
			if (const auto Encoded = EncodeRenderData(*RenderData, Bytes, IsCancelled); !Encoded)
			{
				return std::unexpected(FStaticMeshBuildFailure{FormatStaticMeshCacheCodecError(Encoded.error()), EStaticMeshBuildStage::Render});
			}
			AssetDerivedDataCache::FOperationDiagnostic StoreDiagnostic;
			if (IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Render));
			if (Request.bPersistDerivedData)
				AssetDerivedDataCache::Store(Key, Bytes,
					MaximumStaticMeshPayloadBytes, StoreDiagnostic);
			if (OutCacheErrors) *OutCacheErrors = CollectCacheErrors( LoadDiagnostic, StoreDiagnostic, CacheDecodeCause);
			return RenderData;
		}, ExpectedProviderRegistration);
		if (IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Render));
		if (!Invocation.WasInvoked() || !Invocation.Value)
		{
			const auto Message = Invocation.Status == EFeatureInvokeStatus::Unavailable
				? "The StaticMesh build provider is unavailable."
				: Invocation.Status == EFeatureInvokeStatus::Ambiguous
					? "Multiple StaticMesh build providers are registered."
					: "The StaticMesh build provider invocation failed.";
			return std::unexpected(FStaticMeshBuildFailure{Message, EStaticMeshBuildStage::Render});
		}
		auto Outcome = std::move(*Invocation.Value);
		if (Outcome)
		{
			if (!*Outcome) return std::unexpected(FStaticMeshBuildFailure{"StaticMesh provider returned no render data.", EStaticMeshBuildStage::Validation});
			if (const auto Finalized = FinalizeRenderData(**Outcome, Control); !Finalized)
				return std::unexpected(Finalized.error());
		}
		return Outcome;
#endif
	}

}
