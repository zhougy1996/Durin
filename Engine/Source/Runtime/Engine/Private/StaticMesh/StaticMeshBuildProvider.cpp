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
		case EStaticMeshRecipeError::CollisionMode: Reason = "collision source mode is invalid."; break;
		case EStaticMeshRecipeError::CollisionInput: Reason = "collision input is empty or malformed."; break;
		case EStaticMeshRecipeError::CollisionBuild: Reason = "collision construction failed."; break;
		case EStaticMeshRecipeError::Cancelled: Reason = "recipe was cancelled."; break;
		}
		return std::format("StaticMesh {} recipe: {} (mesh '{}', section '{}', index {}, actual {}, expected {}).",
			Error.Kind == EStaticMeshRecipeKind::Render ? "render" : "collision", Reason,
			Error.MeshName, Error.SectionName, Error.Index, Error.Actual, Error.Expected);
	}

	auto FormatStaticMeshCacheCodecError(const FStaticMeshCacheCodecError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EStaticMeshCacheCodecError::None: return {};
		case EStaticMeshCacheCodecError::RenderPayload:
			return Error.RenderCause ? FormatStaticMeshPayloadError(*Error.RenderCause) : "StaticMesh render payload conversion failed.";
		case EStaticMeshCacheCodecError::CollisionPayload:
			return Error.CollisionCause ? FormatStaticMeshCollisionPayloadError(*Error.CollisionCause) : "StaticMesh collision payload conversion failed.";
		case EStaticMeshCacheCodecError::Archive:
			return std::format("StaticMesh cache payload operation {} failed at byte {} (Archive code {}, path '{}').",
				static_cast<int>(Error.Operation), Error.Actual, Error.ArchiveCode ? static_cast<int>(*Error.ArchiveCode) : -1, Error.ArchivePath);
		case EStaticMeshCacheCodecError::MaterialSlots:
			return std::format("Cached StaticMesh material slot count {} does not match asset metadata count {}.", Error.Actual, Error.Expected);
		case EStaticMeshCacheCodecError::CollisionMetadata:
			return std::format("Cached StaticMesh collision mode/policy ({}/{}) does not match metadata ({}/{}).",
				static_cast<int>(Error.ActualMode), static_cast<int>(Error.ActualPolicy),
				static_cast<int>(Error.ExpectedMode), static_cast<int>(Error.ExpectedPolicy));
		}
		return {};
	}

#if DURIN_WITH_EDITOR
	namespace
	{
		auto BuildCollisionGeometryHash(std::span<const FVector3f> Positions,
			std::span<const uint32> Indices, const std::function<bool()>& ShouldCancel) -> std::optional<FXxHash128>
		{
			FXxHash128Builder Hash;
			FBinaryWriter PositionHeader;
			PositionHeader.WriteU64(Positions.size());
			Hash.Update(PositionHeader.GetBytes());
			for (size_t Begin = 0; Begin < Positions.size(); Begin += 256)
			{
				if (ShouldCancel()) return {};
				FBinaryWriter Block;
				for (size_t Index = Begin; Index < std::min(Begin + 256, Positions.size()); ++Index)
					for (uint32 Axis = 0; Axis < 3; ++Axis)
						Block.WriteU32(std::bit_cast<uint32>(Positions[Index][Axis]));
				Hash.Update(Block.GetBytes());
			}
			FBinaryWriter IndexHeader;
			IndexHeader.WriteU64(Indices.size());
			Hash.Update(IndexHeader.GetBytes());
			for (size_t Begin = 0; Begin < Indices.size(); Begin += 256)
			{
				if (ShouldCancel()) return {};
				FBinaryWriter Block;
				for (size_t Index = Begin; Index < std::min(Begin + 256, Indices.size()); ++Index)
					Block.WriteU32(Indices[Index]);
				Hash.Update(Block.GetBytes());
			}
			return Hash.Finalize();
		}

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

		auto EncodeCollision(const FCollisionGeometryRef& Geometry, EBodySetupCollisionQueryPolicy Policy,
			FByteBuffer& OutBytes, const std::function<bool()>& ShouldCancel) -> std::expected<void, FStaticMeshCacheCodecError>
		{
			FStaticMeshCollisionPayloadData Payload;
			if (const auto Built = MakeStaticMeshCollisionPayloadData(Geometry, Policy, Payload, ShouldCancel); !Built)
				return std::unexpected(FStaticMeshCacheCodecError{.Code = EStaticMeshCacheCodecError::CollisionPayload, .Operation = EStaticMeshCacheCodecOperation::EncodeCollision, .CollisionCause = Built.error()});
			OutBytes.clear();
			FCanonicalMemoryWriter Ar(OutBytes, EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
			Payload.Serialize(Ar, ShouldCancel);
			if (!Ar.IsError()) return {};
			const auto Failure = ArchiveCodecFailure(Ar, EStaticMeshCacheCodecOperation::EncodeCollision);
			OutBytes.clear();
			return Failure;
		}

		auto DecodeCollision(FByteView Bytes, EBodySetupCollisionSourceMode Mode,
			EBodySetupCollisionQueryPolicy Policy, FCollisionGeometryRef& OutGeometry,
			const std::function<bool()>& ShouldCancel) -> std::expected<void, FStaticMeshCacheCodecError>
		{
			FStaticMeshCollisionPayloadData Payload;
			FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
			Payload.Serialize(Ar, ShouldCancel);
			if (Ar.IsError() || !RequireArchiveEnd(Ar)) return ArchiveCodecFailure(Ar, EStaticMeshCacheCodecOperation::DecodeCollision);
			if (Payload.SourceMode != Mode || Payload.QueryPolicy != Policy)
				return std::unexpected(FStaticMeshCacheCodecError{.Code = EStaticMeshCacheCodecError::CollisionMetadata, .Operation = EStaticMeshCacheCodecOperation::DecodeCollision,
					.ActualMode = Payload.SourceMode, .ExpectedMode = Mode, .ActualPolicy = Payload.QueryPolicy, .ExpectedPolicy = Policy});
			if (const auto Built = MakeStaticMeshCollisionGeometry(Payload, OutGeometry, ShouldCancel); !Built)
				return std::unexpected(FStaticMeshCacheCodecError{.Code = EStaticMeshCacheCodecError::CollisionPayload, .Operation = EStaticMeshCacheCodecOperation::DecodeCollision, .CollisionCause = Built.error()});
			return {};
		}

		auto CollectCacheErrors(EStaticMeshRecipeKind Kind,
			const AssetDerivedDataCache::FOperationDiagnostic& Read,
			const AssetDerivedDataCache::FOperationDiagnostic& Write,
			const std::optional<FStaticMeshCacheCodecError>& Decode) -> std::vector<FStaticMeshCacheError>
		{
			std::vector<FStaticMeshCacheError> Errors;
			if (Read.Code != EAssetCacheError::None)
				Errors.emplace_back(Kind, EStaticMeshCacheOperation::Read, FormatAssetCacheDiagnostic(Read));
			if (Decode)
				Errors.emplace_back(Kind, EStaticMeshCacheOperation::Decode, FormatStaticMeshCacheCodecError(*Decode));
			if (Write.Code != EAssetCacheError::None)
				Errors.emplace_back(Kind, EStaticMeshCacheOperation::Write, FormatAssetCacheDiagnostic(Write));
			return Errors;
		}

	}

#endif
	auto FStaticMeshBuilder::Build(
		FStaticMeshBuildRequest Request,
		const FStaticMeshBuildExecutionControl& Control, std::vector<FStaticMeshCacheError>* OutCacheErrors,
		uint64* OutProviderRegistration) -> std::expected<std::unique_ptr<FStaticMeshRenderData>, FStaticMeshBuildFailure>
	{
		if (OutCacheErrors) OutCacheErrors->clear();
		if (OutProviderRegistration) *OutProviderRegistration = 0;
		if (Control.IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Render));
		if (!Request.Source.IsValid() || !std::isfinite(Request.Reconciliation.NormalizedSize)
			|| Request.Reconciliation.NormalizedSize <= 0)
			return std::unexpected(FStaticMeshBuildFailure{"StaticMesh render source or normalization is invalid.", EStaticMeshBuildStage::Source});
		FStaticMeshBuildMemoryEstimate SourceMemory{Control.MaximumWorkingSetBytes};
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
				return std::unexpected(FStaticMeshBuildFailure{std::format("Invalid StaticMesh provider '{}' (render version {}, collision version {}).",
						Descriptor.ProducerIdentity, Descriptor.RenderBuilderVersion, Descriptor.CollisionBuilderVersion), EStaticMeshBuildStage::Render});
			}
			FStaticMeshBuildKeyInput KeyInput{
				.SourceHash = Request.Source.GetIdentity(),
				.ReconciliationHash = BuildStaticMeshReconciliationHash(
					Request.Reconciliation.MaterialSlots,
					Request.Reconciliation.NormalizedSize),
				.BuilderVersion = Descriptor.RenderBuilderVersion,
				.TargetPlatform = EStaticMeshTargetPlatform::Win64};
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
			if (OutCacheErrors) *OutCacheErrors = CollectCacheErrors(EStaticMeshRecipeKind::Render, LoadDiagnostic, StoreDiagnostic, CacheDecodeCause);
			return RenderData;
		}, Control.ExpectedProviderRegistration);
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
		if (Outcome && OutProviderRegistration) *OutProviderRegistration = Invocation.RegistrationIdentity;
		return Outcome;
#endif
	}

	auto FStaticMeshCollisionBuilder::Capture(const FStaticMeshRenderData& Render,
		EBodySetupCollisionSourceMode Mode, EBodySetupCollisionQueryPolicy Policy,
		bool bPersistDerivedData) -> FStaticMeshCollisionBuildRequest
	{
		FStaticMeshCollisionBuildRequest Request{.Mode = Mode, .Policy = Policy, .bPersistDerivedData = bPersistDerivedData};
		if (Mode != EBodySetupCollisionSourceMode::None && !Render.LODResources.empty())
		{
			Request.Positions = Render.LODResources.front().VertexBuffers.PositionVertexBuffer.GetPositions();
			Request.Indices = Render.LODResources.front().IndexBuffer.GetIndices();
		}
		return Request;
	}

	auto FStaticMeshCollisionBuilder::Build(const FStaticMeshCollisionBuildRequest& Request,
		const FStaticMeshBuildExecutionControl& Control) -> std::expected<FStaticMeshCollisionBuildProduct, FStaticMeshBuildFailure>
	{
		const auto Mode = Request.Mode;
		const auto Policy = Request.Policy;
		const auto bPersistDerivedData = Request.bPersistDerivedData;
		bool bCancelled = false;
		const auto IsCancelled = [&] {
			bCancelled = bCancelled || Control.IsCancelled();
			return bCancelled;
		};
		if (IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Collision));
		if (Mode == EBodySetupCollisionSourceMode::None)
		{
			if (IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Collision));
			return FStaticMeshCollisionBuildProduct{};
		}
#if !DURIN_WITH_EDITOR
		return std::unexpected(FStaticMeshBuildFailure{"StaticMesh build orchestration is unavailable outside editor builds.", EStaticMeshBuildStage::Collision});
#else
		const auto& Positions = Request.Positions;
		const auto& Indices = Request.Indices;
		FStaticMeshBuildMemoryEstimate Memory{Control.MaximumWorkingSetBytes};
		if (!Memory.Add(Positions.capacity(), 512) || !Memory.Add(Indices.capacity(), 192))
			return std::unexpected(FStaticMeshBuildFailure{"StaticMesh collision snapshot and working set exceed the reservation.", EStaticMeshBuildStage::Collision});
		if (Positions.empty() || Indices.empty() || Indices.size() % 3 != 0)
		{
			return std::unexpected(FStaticMeshBuildFailure{std::format("StaticMesh collision source is malformed ({} vertices, {} indices).", Positions.size(), Indices.size()), EStaticMeshBuildStage::Collision});
		}
		auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<
			IStaticMeshBuildProvider>([&](IStaticMeshBuildProvider& Provider) -> std::expected<FStaticMeshCollisionBuildProduct, FStaticMeshBuildFailure> {
			const FStaticMeshBuildProviderDescriptor Descriptor = Provider.GetDescriptor();
			if (!Descriptor.IsValid())
			{
				return std::unexpected(FStaticMeshBuildFailure{std::format("Invalid StaticMesh provider '{}' (render version {}, collision version {}).",
						Descriptor.ProducerIdentity, Descriptor.RenderBuilderVersion, Descriptor.CollisionBuilderVersion), EStaticMeshBuildStage::Collision});
			}
			const auto GeometryHash = BuildCollisionGeometryHash(Positions, Indices, IsCancelled);
			if (!GeometryHash) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Collision));
			const FStaticMeshCollisionBuildKeyInput KeyInput{
				.GeometryHash = *GeometryHash,
				.SourceMode = Mode,
				.QueryPolicy = Policy,
				.BuilderVersion = Descriptor.CollisionBuilderVersion,
				.TargetPlatform = EStaticMeshTargetPlatform::Win64};
			const auto KeyResult = BuildStaticMeshCollisionDerivedDataKey(KeyInput);
			if (!KeyResult)
			{
				return std::unexpected(FStaticMeshBuildFailure{FormatStaticMeshBuildKeyError(KeyResult.error()), EStaticMeshBuildStage::Collision});
			}
			const FCacheKeyProxy Key = *KeyResult;
			FByteBuffer Bytes;
			AssetDerivedDataCache::FOperationDiagnostic LoadDiagnostic;
			FCollisionGeometryRef Geometry;
			AssetDerivedDataCache::FOperationDiagnostic StoreDiagnostic;
			bool bCacheHit = false;
			std::optional<FStaticMeshCacheCodecError> CacheDecodeCause;
			if (AssetDerivedDataCache::Load(Key,
				std::min(MaximumStaticMeshCollisionPayloadBytes, Control.MaximumWorkingSetBytes / 16), Bytes, LoadDiagnostic)
				== AssetDerivedDataCache::ELoadResult::Hit)
				{
				const auto Decoded = DecodeCollision(Bytes, Mode, Policy, Geometry, IsCancelled);
				bCacheHit = bool(Decoded);
				if (!Decoded)
				{
					CacheDecodeCause = Decoded.error();
				}
			}
			if (IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Collision));
			if (!bCacheHit)
			{
				auto RecipeOutcome = Provider.BuildCollision(
					{Positions, Indices, Mode, Policy}, Control);
				if (!RecipeOutcome || !RecipeOutcome->Geometry)
				{
					if (RecipeOutcome) return std::unexpected(FStaticMeshBuildFailure{"StaticMesh provider did not return a complete product.", EStaticMeshBuildStage::Collision});
					return std::unexpected(RecipeOutcome.error().Code == EStaticMeshRecipeError::Cancelled
						? FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Collision, FormatStaticMeshRecipeError(RecipeOutcome.error()))
						: FStaticMeshBuildFailure{FormatStaticMeshRecipeError(RecipeOutcome.error()), EStaticMeshBuildStage::Collision});
				}
				Geometry = std::move(RecipeOutcome->Geometry);
				if (IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Collision));
				if (const auto Encoded = EncodeCollision(Geometry, Policy, Bytes, IsCancelled); !Encoded)
				{
					return std::unexpected(FStaticMeshBuildFailure{FormatStaticMeshCacheCodecError(Encoded.error()), EStaticMeshBuildStage::Collision});
				}
				if (IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Collision));
				if (bPersistDerivedData)
					AssetDerivedDataCache::Store(Key, Bytes,
						MaximumStaticMeshCollisionPayloadBytes, StoreDiagnostic);
			}
			FStaticMeshCollisionBuildProduct OutProduct;
			if (Mode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0)
				OutProduct.Simple = Geometry;
			else OutProduct.Complex = Geometry;
			OutProduct.CacheErrors = CollectCacheErrors(EStaticMeshRecipeKind::Collision, LoadDiagnostic, StoreDiagnostic, CacheDecodeCause);
			return OutProduct;
		}, Control.ExpectedProviderRegistration);
		if (IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Collision));
		if (!Invocation.WasInvoked() || !Invocation.Value)
		{
			const auto Message = Invocation.Status == EFeatureInvokeStatus::Unavailable
				? "The StaticMesh build provider is unavailable."
				: Invocation.Status == EFeatureInvokeStatus::Ambiguous
					? "Multiple StaticMesh build providers are registered."
					: "The StaticMesh build provider invocation failed.";
			return std::unexpected(FStaticMeshBuildFailure{Message, EStaticMeshBuildStage::Collision});
		}
		auto Outcome = std::move(*Invocation.Value);
		return Outcome;
#endif
	}

}
