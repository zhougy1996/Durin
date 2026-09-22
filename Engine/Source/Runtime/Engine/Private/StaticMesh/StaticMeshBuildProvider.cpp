#include "StaticMesh/StaticMeshBuild.h"

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

	auto FormatStaticMeshDerivedDataError(const FStaticMeshDerivedDataError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EStaticMeshDerivedDataError::None: return {};
		case EStaticMeshDerivedDataError::Cancelled: return "StaticMesh build was cancelled.";
		case EStaticMeshDerivedDataError::Unavailable: return "StaticMesh build orchestration is unavailable outside editor builds.";
		case EStaticMeshDerivedDataError::ProviderDescriptor: return "The StaticMesh build provider descriptor is invalid.";
		case EStaticMeshDerivedDataError::ProviderInvocation:
			if (Error.InvocationStatus == EFeatureInvokeStatus::Unavailable) return "The StaticMesh build provider is unavailable.";
			if (Error.InvocationStatus == EFeatureInvokeStatus::Ambiguous) return "Multiple StaticMesh build providers are registered.";
			return "The StaticMesh build provider invocation failed.";
		case EStaticMeshDerivedDataError::Key:
#if DURIN_WITH_EDITOR
			if (Error.KeyCause) return FormatStaticMeshBuildKeyError(*Error.KeyCause);
#endif
			return "StaticMesh derived-data key construction failed.";
		case EStaticMeshDerivedDataError::Source:
			return Error.SourceCause ? FormatStaticMeshSourceError(*Error.SourceCause) : "StaticMesh source acquisition failed.";
		case EStaticMeshDerivedDataError::Recipe:
			return Error.RecipeCause ? FormatStaticMeshRecipeError(*Error.RecipeCause) : "StaticMesh recipe failed.";
		case EStaticMeshDerivedDataError::Payload:
			return Error.PayloadCause ? FormatStaticMeshCacheCodecError(*Error.PayloadCause) : "StaticMesh payload encoding failed.";
		case EStaticMeshDerivedDataError::MissingLOD: return "StaticMesh has no LOD 0 collision source.";
		case EStaticMeshDerivedDataError::InvalidGeometry:
			return std::format("StaticMesh LOD 0 collision source is empty or malformed ({} vertices, {} indices).", Error.VertexCount, Error.IndexCount);
		case EStaticMeshDerivedDataError::InvalidProduct: return "StaticMesh provider did not return a complete product.";
		}
		return {};
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
			const std::function<bool()>& ShouldCancel) -> std::unique_ptr<FStaticMeshRenderData>
		{
			auto RenderData = std::make_unique<FStaticMeshRenderData>();
			RenderData->LocalBounds = Product.LocalBounds;
			RenderData->MaterialSlots.reserve(Product.MaterialSlots.size());
			for (const auto& Slot : Product.MaterialSlots)
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

		auto SetInvocationError(EFeatureInvokeStatus Status, FStaticMeshDerivedDataError& Error) -> void
		{
			Error.InvocationStatus = Status;
			if (Error.Code == EStaticMeshDerivedDataError::None) Error.Code = EStaticMeshDerivedDataError::ProviderInvocation;
		}

	}

#endif
	static auto BuildStaticMeshDerivedDataInternal(
		FStaticMeshBuildRequest Request,
		FStaticMeshBuildResult& OutProduct,
		FStaticMeshDerivedDataError& OutError, const FStaticMeshBuildExecutionControl& Control) -> bool
	{
		OutProduct = {};
		const auto IsCancelled = [&] {
			if (OutError.Code != EStaticMeshDerivedDataError::Cancelled && !Control.IsCancelled()) return false;
			OutError.Code = EStaticMeshDerivedDataError::Cancelled;
			return true;
		};
		if (IsCancelled()) return false;
#if !DURIN_WITH_EDITOR
		OutError.Code = EStaticMeshDerivedDataError::Unavailable;
		return false;
#else
		const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<
			IStaticMeshBuildProvider>([&](IStaticMeshBuildProvider& Provider) {
			const FStaticMeshBuildProviderDescriptor Descriptor = Provider.GetDescriptor();
			if (!Descriptor.IsValid())
			{
				OutError = {.Code = EStaticMeshDerivedDataError::ProviderDescriptor, .Descriptor = Descriptor};
				return false;
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
				OutError = {.Code = EStaticMeshDerivedDataError::Key, .KeyCause = KeyResult.error()};
				return false;
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
					OutProduct = {
						.RenderData = std::move(RenderData),
						.MaterialSlots = Request.Reconciliation.MaterialSlots,
						.NormalizedSize = Request.Reconciliation.NormalizedSize,
						.DerivedDataKey = Key,
						.Origin = EStaticMeshBuildOrigin::CacheHit,
						.Descriptor = Descriptor,
						.CacheReadNanoseconds = LoadDiagnostic.DurationNanoseconds,
						.PayloadBytes = Bytes.size()};
					return true;
				}
				CacheDecodeCause = Decoded.error();
			}
			if (IsCancelled()) return false;
			auto Decoded = Request.Source.AcquireGeometry(IsCancelled);
			if (!Decoded)
				OutError = {.Code = Decoded.error().Code == EStaticMeshSourceError::Cancelled
					? EStaticMeshDerivedDataError::Cancelled : EStaticMeshDerivedDataError::Source, .SourceCause = Decoded.error()};
			if (IsCancelled() || !Decoded) return false;
			std::vector<FStaticMeshRecipeMaterialSlot> RecipeSlots;
			for (const auto& Slot : Request.Reconciliation.MaterialSlots)
				RecipeSlots.push_back({Slot.Name, Slot.SourceName, Slot.SourceMaterialIndex});
			auto RecipeOutcome = Provider.BuildRender({
				.Geometry = std::move(*Decoded),
				.PreviousMaterialSlots = RecipeSlots,
				.NormalizedSize = Request.Reconciliation.NormalizedSize}, Control);
			if (!RecipeOutcome)
			{
				OutError = {.Code = (!RecipeOutcome && RecipeOutcome.error().Code == EStaticMeshRecipeError::Cancelled)
					? EStaticMeshDerivedDataError::Cancelled : EStaticMeshDerivedDataError::Recipe, .RecipeCause = RecipeOutcome.error()};
				return false;
			}
			if (IsCancelled()) return false;
			auto& RecipeProduct = *RecipeOutcome;
			auto RenderData = AssembleRenderData(RecipeProduct, IsCancelled);
			if (!RenderData) return false;
			if (const auto Encoded = EncodeRenderData(*RenderData, Bytes, IsCancelled); !Encoded)
			{
				OutError = {.Code = EStaticMeshDerivedDataError::Payload, .PayloadCause = Encoded.error()};
				return false;
			}
			std::vector<FMeshMaterialSlotDefinition> MaterialSlots;
			MaterialSlots.reserve(RecipeProduct.MaterialSlots.size());
			for (size_t Index = 0; Index < RecipeProduct.MaterialSlots.size(); ++Index)
			{
				const auto& Slot = RecipeProduct.MaterialSlots[Index];
				MaterialSlots.push_back({.Name = Slot.Name, .SourceName = Slot.SourceName,
					.SourceMaterialIndex = Slot.SourceMaterialIndex});
				if (Index < Request.Reconciliation.MaterialSlots.size())
					MaterialSlots.back().DefaultMaterial =
						Request.Reconciliation.MaterialSlots[Index].DefaultMaterial;
			}
			KeyInput.ReconciliationHash = BuildStaticMeshReconciliationHash(
				MaterialSlots, Request.Reconciliation.NormalizedSize);
			KeyResult = BuildStaticMeshDerivedDataKey(KeyInput);
			if (!KeyResult)
			{
				OutError = {.Code = EStaticMeshDerivedDataError::Key, .KeyCause = KeyResult.error()};
				return false;
			}
			Key = *KeyResult;
			AssetDerivedDataCache::FOperationDiagnostic StoreDiagnostic;
			if (IsCancelled()) return false;
			if (Request.bPersistDerivedData)
				AssetDerivedDataCache::Store(Key, Bytes,
					MaximumStaticMeshPayloadBytes, StoreDiagnostic);
			OutProduct = {
				.RenderData = std::move(RenderData),
				.MaterialSlots = std::move(MaterialSlots),
				.NormalizedSize = Request.Reconciliation.NormalizedSize,
				.DerivedDataKey = Key,
				.bSlotMetadataChanged = RecipeProduct.bSlotMetadataChanged,
				.Origin = EStaticMeshBuildOrigin::Rebuilt,
				.Descriptor = Descriptor,
				.CacheReadNanoseconds = LoadDiagnostic.DurationNanoseconds,
				.CacheWriteNanoseconds = StoreDiagnostic.DurationNanoseconds,
				.PayloadBytes = Bytes.size(),
				.CacheDiagnostics = {std::move(LoadDiagnostic), std::move(StoreDiagnostic), std::move(CacheDecodeCause)}};
			return true;
		}, Control.ExpectedProviderRegistration);
		if (Invocation.Status == EFeatureInvokeStatus::Invoked
			&& Invocation.Value.has_value() && *Invocation.Value)
		{
			OutProduct.ProviderRegistration = Invocation.RegistrationIdentity;
			OutError = {};
			return true;
		}
		OutProduct = {};
		SetInvocationError(Invocation.Status, OutError);
		return false;
#endif
	}

	static auto BuildStaticMeshCollisionDerivedDataInternal(
		const FStaticMeshRenderData& RenderData,
		EBodySetupCollisionSourceMode Mode,
		EBodySetupCollisionQueryPolicy Policy,
		FStaticMeshCollisionBuildResult& OutProduct,
		FStaticMeshDerivedDataError& OutError,
		bool bPersistDerivedData, const FStaticMeshBuildExecutionControl& Control) -> bool
	{
		OutProduct = {};
		const auto IsCancelled = [&] {
			if (OutError.Code != EStaticMeshDerivedDataError::Cancelled && !Control.IsCancelled()) return false;
			OutError.Code = EStaticMeshDerivedDataError::Cancelled;
			return true;
		};
		if (IsCancelled()) return false;
		if (Mode == EBodySetupCollisionSourceMode::None)
		{
			OutError = {};
			return true;
		}
#if !DURIN_WITH_EDITOR
		OutError.Code = EStaticMeshDerivedDataError::Unavailable;
		return false;
#else
		if (RenderData.LODResources.empty())
		{
			OutError.Code = EStaticMeshDerivedDataError::MissingLOD;
			return false;
		}
		const FStaticMeshLODResources& LOD = RenderData.LODResources.front();
		const auto& Positions = LOD.VertexBuffers.PositionVertexBuffer.GetPositions();
		const auto& Indices = LOD.IndexBuffer.GetIndices();
		if (Positions.empty() || Indices.empty() || Indices.size() % 3 != 0)
		{
			OutError = {.Code = EStaticMeshDerivedDataError::InvalidGeometry, .VertexCount = Positions.size(), .IndexCount = Indices.size()};
			return false;
		}
		const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<
			IStaticMeshBuildProvider>([&](IStaticMeshBuildProvider& Provider) {
			const FStaticMeshBuildProviderDescriptor Descriptor = Provider.GetDescriptor();
			if (!Descriptor.IsValid())
			{
				OutError = {.Code = EStaticMeshDerivedDataError::ProviderDescriptor, .Descriptor = Descriptor};
				return false;
			}
			const auto GeometryHash = BuildCollisionGeometryHash(Positions, Indices, IsCancelled);
			if (!GeometryHash) return false;
			const FStaticMeshCollisionBuildKeyInput KeyInput{
				.GeometryHash = *GeometryHash,
				.SourceMode = Mode,
				.QueryPolicy = Policy,
				.BuilderVersion = Descriptor.CollisionBuilderVersion,
				.TargetPlatform = EStaticMeshTargetPlatform::Win64};
			const auto KeyResult = BuildStaticMeshCollisionDerivedDataKey(KeyInput);
			if (!KeyResult)
			{
				OutError = {.Code = EStaticMeshDerivedDataError::Key, .KeyCause = KeyResult.error()};
				return false;
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
			if (IsCancelled()) return false;
			if (!bCacheHit)
			{
				auto RecipeOutcome = Provider.BuildCollision(
					{Positions, Indices, Mode, Policy}, Control);
				if (!RecipeOutcome || !RecipeOutcome->Geometry)
				{
					OutError.Code = RecipeOutcome ? EStaticMeshDerivedDataError::InvalidProduct
						: (!RecipeOutcome && RecipeOutcome.error().Code == EStaticMeshRecipeError::Cancelled) ? EStaticMeshDerivedDataError::Cancelled : EStaticMeshDerivedDataError::Recipe;
					if (!RecipeOutcome) OutError.RecipeCause = RecipeOutcome.error();
					return false;
				}
				Geometry = std::move(RecipeOutcome->Geometry);
				if (IsCancelled()) return false;
				if (const auto Encoded = EncodeCollision(Geometry, Policy, Bytes, IsCancelled); !Encoded)
				{
					OutError = {.Code = EStaticMeshDerivedDataError::Payload, .PayloadCause = Encoded.error()};
					return false;
				}
				OutError = {};
				if (IsCancelled()) return false;
				if (bPersistDerivedData)
					AssetDerivedDataCache::Store(Key, Bytes,
						MaximumStaticMeshCollisionPayloadBytes, StoreDiagnostic);
			}
			if (Mode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0)
				OutProduct.Simple = Geometry;
			else OutProduct.Complex = Geometry;
			OutProduct.Origin = bCacheHit
				? EStaticMeshBuildOrigin::CacheHit
				: EStaticMeshBuildOrigin::Rebuilt;
			OutProduct.DerivedDataKey = Key;
			OutProduct.PayloadBytes = Bytes.size();
			OutProduct.CacheDiagnostics = {LoadDiagnostic, StoreDiagnostic, std::move(CacheDecodeCause)};
			OutProduct.Descriptor = Descriptor;
			OutProduct.CacheReadNanoseconds = LoadDiagnostic.DurationNanoseconds;
			OutProduct.CacheWriteNanoseconds = StoreDiagnostic.DurationNanoseconds;
			return true;
		}, Control.ExpectedProviderRegistration);
		if (Invocation.Status == EFeatureInvokeStatus::Invoked
			&& Invocation.Value.has_value() && *Invocation.Value)
		{
			OutProduct.ProviderRegistration = Invocation.RegistrationIdentity;
			OutError = {};
			return true;
		}
		OutProduct = {};
		SetInvocationError(Invocation.Status, OutError);
		return false;
#endif
	}

	auto BuildStaticMeshDerivedData(FStaticMeshBuildRequest Request,
		FStaticMeshBuildResult& OutProduct,
		const FStaticMeshBuildExecutionControl& Control) -> std::expected<void, FStaticMeshDerivedDataError>
	{
		FStaticMeshDerivedDataError Error;
		const bool bSucceeded = BuildStaticMeshDerivedDataInternal(std::move(Request), OutProduct, Error, Control);
		if (Control.IsCancelled()) Error.Code = EStaticMeshDerivedDataError::Cancelled;
		else if (!bSucceeded && Error.Code == EStaticMeshDerivedDataError::None) Error.Code = EStaticMeshDerivedDataError::InvalidProduct;
		if (Error.Code == EStaticMeshDerivedDataError::None) return {};
		OutProduct = {};
		return std::unexpected(std::move(Error));
	}

	auto BuildStaticMeshCollisionDerivedData(const FStaticMeshRenderData& RenderData,
		EBodySetupCollisionSourceMode Mode, EBodySetupCollisionQueryPolicy Policy,
		FStaticMeshCollisionBuildResult& OutProduct, bool bPersistDerivedData,
		const FStaticMeshBuildExecutionControl& Control) -> std::expected<void, FStaticMeshDerivedDataError>
	{
		FStaticMeshDerivedDataError Error;
		const bool bSucceeded = BuildStaticMeshCollisionDerivedDataInternal(RenderData, Mode, Policy, OutProduct, Error, bPersistDerivedData, Control);
		if (Control.IsCancelled()) Error.Code = EStaticMeshDerivedDataError::Cancelled;
		else if (!bSucceeded && Error.Code == EStaticMeshDerivedDataError::None) Error.Code = EStaticMeshDerivedDataError::InvalidProduct;
		Error.Kind = EStaticMeshRecipeKind::Collision;
		if (Error.Code == EStaticMeshDerivedDataError::None) return {};
		OutProduct = {};
		return std::unexpected(std::move(Error));
	}
}
