#include "StaticMesh/StaticMeshBuild.h"

#include "Asset/AssetDerivedDataCache.h"
#include "Serialization/Archive.h"
#include "Serialization/BinaryFormat.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshDerivedDataKey.h"

namespace Durin
{
#if DURIN_WITH_EDITOR
	namespace
	{
		auto BuildCollisionGeometryHash(
			std::span<const FVector3f> Positions,
			std::span<const uint32> Indices) -> FXxHash128
		{
			FBinaryWriter Writer;
			Writer.WriteU64(Positions.size());
			for (const FVector3f& Position : Positions)
				for (uint32 Axis = 0; Axis < 3; ++Axis)
					Writer.WriteU32(std::bit_cast<uint32>(Position[Axis]));
			Writer.WriteU64(Indices.size());
			for (uint32 Index : Indices) Writer.WriteU32(Index);
			return FXxHash128::HashBuffer(Writer.GetBytes());
		}

		auto RestoreRuntimeMetadata(
			std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
			FStaticMeshRenderData& RenderData,
			std::string& OutError) -> bool
		{
			if (RenderData.MaterialSlots.size() != MaterialSlots.size())
			{
				OutError = "Cached StaticMesh material slot count does not match asset metadata.";
				return false;
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
			OutError.clear();
			return true;
		}

		auto EncodeRenderData(
			const FStaticMeshRenderData& RenderData,
			FByteArray& OutBytes,
			std::string& OutError) -> bool
		{
			FStaticMeshPayloadData Payload;
			if (!MakeStaticMeshPayloadData(RenderData, Payload, OutError)) return false;
			OutBytes.clear();
			FCanonicalMemoryWriter Ar(OutBytes, EArchivePurpose::DerivedDataPayload);
			Payload.Serialize(Ar, EStaticMeshTargetPlatform::Win64);
			if (!Ar.HasError()) return true;
			OutError = Ar.GetFailure()->Message;
			OutBytes.clear();
			return false;
		}

		auto DecodeRenderData(
			std::span<const std::byte> Bytes,
			std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
			std::unique_ptr<FStaticMeshRenderData>& OutRenderData,
			std::string& OutError) -> bool
		{
			FStaticMeshPayloadData Payload;
			FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::DerivedDataPayload);
			Payload.Serialize(Ar, EStaticMeshTargetPlatform::Win64);
			if (Ar.HasError() || !RequireArchiveEnd(Ar))
			{
				OutError = Ar.GetFailure() ? Ar.GetFailure()->Message
					: "StaticMesh payload has trailing bytes.";
				return false;
			}
			return MakeStaticMeshRenderData(Payload, OutRenderData, OutError)
				&& RestoreRuntimeMetadata(MaterialSlots, *OutRenderData, OutError);
		}

		auto EncodeCollision(
			const FCollisionGeometryRef& Geometry,
			EBodySetupCollisionQueryPolicy Policy,
			FByteArray& OutBytes,
			std::string& OutError) -> bool
		{
			FStaticMeshCollisionPayloadData Payload;
			if (!MakeStaticMeshCollisionPayloadData(
				Geometry, Policy, Payload, OutError)) return false;
			OutBytes.clear();
			FCanonicalMemoryWriter Ar(OutBytes, EArchivePurpose::DerivedDataPayload);
			Payload.Serialize(Ar, EStaticMeshTargetPlatform::Win64);
			if (!Ar.HasError()) return true;
			OutError = Ar.GetFailure()->Message;
			OutBytes.clear();
			return false;
		}

		auto DecodeCollision(
			std::span<const std::byte> Bytes,
			EBodySetupCollisionSourceMode Mode,
			EBodySetupCollisionQueryPolicy Policy,
			FCollisionGeometryRef& OutGeometry,
			std::string& OutError) -> bool
		{
			FStaticMeshCollisionPayloadData Payload;
			FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::DerivedDataPayload);
			Payload.Serialize(Ar, EStaticMeshTargetPlatform::Win64);
			if (Ar.HasError() || !RequireArchiveEnd(Ar)
				|| Payload.SourceMode != Mode || Payload.QueryPolicy != Policy)
			{
				OutError = Ar.GetFailure() ? Ar.GetFailure()->Message
					: "StaticMesh collision payload is incompatible or has trailing bytes.";
				return false;
			}
			return MakeStaticMeshCollisionGeometry(Payload, OutGeometry, OutError);
		}

		auto SetInvocationError(
			EFeatureInvokeStatus Status,
			std::string& OutError) -> void
		{
			if (Status == EFeatureInvokeStatus::Unavailable)
				OutError = "The StaticMesh build provider is unavailable.";
			else if (Status == EFeatureInvokeStatus::Ambiguous)
				OutError = "Multiple StaticMesh build providers are registered.";
			else if (Status == EFeatureInvokeStatus::VisitorFailed)
				OutError = "The StaticMesh build provider invocation failed.";
			else if (OutError.empty())
				OutError = "The StaticMesh build provider failed without a diagnostic.";
		}
	}

#endif
	auto BuildStaticMeshDerivedData(
		FStaticMeshBuildRequest Request,
		FStaticMeshBuildResult& OutProduct,
		std::string& OutError) -> bool
	{
		OutProduct = {};
#if !DURIN_WITH_EDITOR
		OutError = "StaticMesh authored build orchestration is unavailable outside editor builds.";
		return false;
#else
		// Fresh imports have decoded arrays but no captured bulk identity yet.
		// Metadata-only PostLoad requests must not read authored bulk before Get.
		if (!Request.ImportedData.Meshes.empty()
			&& !Request.ImportedData.CaptureDecodedData(OutError)) return false;
		const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<
			IStaticMeshBuildProvider>([&](IStaticMeshBuildProvider& Provider) {
			const FStaticMeshBuildProviderDescriptor Descriptor = Provider.GetDescriptor();
			if (!Descriptor.IsValid())
			{
				OutError = "The StaticMesh build provider descriptor is invalid.";
				return false;
			}
			FStaticMeshBuildKeyInput KeyInput{
				.ImportedDataHash = Request.ImportedData.GetIdentity(),
				.ReconciliationHash = BuildStaticMeshReconciliationHash(
					Request.Reconciliation.MaterialSlots,
					Request.Reconciliation.NormalizedSize),
				.BuilderVersion = Descriptor.RenderBuilderVersion,
				.TargetPlatform = EStaticMeshTargetPlatform::Win64};
			FCacheKeyProxy Key = BuildStaticMeshDerivedDataKey(KeyInput, OutError);
			if (!Key.IsValid()) return false;
			AssetDerivedDataCache::FOperationDiagnostic LoadDiagnostic;
			FByteArray Bytes;
			if (AssetDerivedDataCache::Load(
				Key, MaximumStaticMeshPayloadBytes,
				Bytes, LoadDiagnostic) == AssetDerivedDataCache::ELoadResult::Hit)
			{
				std::unique_ptr<FStaticMeshRenderData> RenderData;
				if (DecodeRenderData(Bytes, Request.Reconciliation.MaterialSlots,
					RenderData, LoadDiagnostic.Message))
				{
					OutProduct = {
						.RenderData = std::move(RenderData),
						.MaterialSlots = Request.Reconciliation.MaterialSlots,
						.ImportedData = std::move(Request.ImportedData),
						.NormalizedSize = Request.Reconciliation.NormalizedSize,
						.DerivedDataKey = Key,
						.Origin = EStaticMeshBuildOrigin::CacheHit,
						.Descriptor = Descriptor,
						.CacheReadNanoseconds = LoadDiagnostic.DurationNanoseconds,
						.PayloadBytes = Bytes.size()};
					return true;
				}
			}
			FStaticMeshImportedData Decoded = Request.ImportedData.Decode(OutError);
			if (!OutError.empty()) return false;
			std::vector<FStaticMeshRecipeMaterialSlot> RecipeSlots;
			for (const auto& Slot : Request.Reconciliation.MaterialSlots)
				RecipeSlots.push_back({Slot.Name, Slot.SourceName, Slot.SourceMaterialIndex});
			FStaticMeshRecipeBuildProduct RecipeProduct;
			if (!Provider.BuildRender({
				.ImportedData = std::cref(Decoded),
				.PreviousMaterialSlots = RecipeSlots,
				.NormalizedSize = Request.Reconciliation.NormalizedSize}, RecipeProduct, OutError)) return false;
			if (!RecipeProduct.RenderData
				|| !EncodeRenderData(*RecipeProduct.RenderData, Bytes, OutError)) return false;
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
			Key = BuildStaticMeshDerivedDataKey(KeyInput, OutError);
			if (!Key.IsValid()) return false;
			AssetDerivedDataCache::FOperationDiagnostic StoreDiagnostic;
			if (Request.bPersistDerivedData)
				AssetDerivedDataCache::Store(Key, Bytes,
					MaximumStaticMeshPayloadBytes, StoreDiagnostic);
			OutProduct = {
				.RenderData = std::move(RecipeProduct.RenderData),
				.MaterialSlots = std::move(MaterialSlots),
				.ImportedData = std::move(Request.ImportedData),
				.NormalizedSize = Request.Reconciliation.NormalizedSize,
				.DerivedDataKey = Key,
				.bSlotMetadataChanged = RecipeProduct.bSlotMetadataChanged,
				.Origin = EStaticMeshBuildOrigin::Rebuilt,
				.Descriptor = Descriptor,
				.CacheReadNanoseconds = LoadDiagnostic.DurationNanoseconds,
				.CacheWriteNanoseconds = StoreDiagnostic.DurationNanoseconds,
				.PayloadBytes = Bytes.size(),
				.DiagnosticMessage = AssetDerivedDataCache::CombineDiagnostics(
					LoadDiagnostic, StoreDiagnostic)};
			return true;
		});
		if (Invocation.Status == EFeatureInvokeStatus::Invoked
			&& Invocation.Value.has_value() && *Invocation.Value)
		{
			OutError.clear();
			return true;
		}
		OutProduct = {};
		SetInvocationError(Invocation.Status, OutError);
		return false;
#endif
	}

	auto BuildStaticMeshCollisionDerivedData(
		const FStaticMeshRenderData& RenderData,
		EBodySetupCollisionSourceMode Mode,
		EBodySetupCollisionQueryPolicy Policy,
		FStaticMeshCollisionBuildResult& OutProduct,
		std::string& OutError,
		bool bPersistDerivedData) -> bool
	{
		OutProduct = {};
		if (Mode == EBodySetupCollisionSourceMode::None)
		{
			OutError.clear();
			return true;
		}
#if !DURIN_WITH_EDITOR
		OutError = "StaticMesh collision build orchestration is unavailable outside editor builds.";
		return false;
#else
		if (RenderData.LODResources.empty())
		{
			OutError = "StaticMesh has no LOD 0 collision source.";
			return false;
		}
		const FStaticMeshLODResources& LOD = RenderData.LODResources.front();
		const auto& Positions = LOD.VertexBuffers.PositionVertexBuffer.GetPositions();
		const auto& Indices = LOD.IndexBuffer.GetIndices();
		if (Positions.empty() || Indices.empty() || Indices.size() % 3 != 0)
		{
			OutError = "StaticMesh LOD 0 collision source is empty or malformed.";
			return false;
		}
		const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<
			IStaticMeshBuildProvider>([&](IStaticMeshBuildProvider& Provider) {
			const FStaticMeshBuildProviderDescriptor Descriptor = Provider.GetDescriptor();
			if (!Descriptor.IsValid()) return false;
			const FStaticMeshCollisionBuildKeyInput KeyInput{
				.GeometryHash = BuildCollisionGeometryHash(Positions, Indices),
				.SourceMode = Mode,
				.QueryPolicy = Policy,
				.BuilderVersion = Descriptor.CollisionBuilderVersion,
				.TargetPlatform = EStaticMeshTargetPlatform::Win64};
			const FCacheKeyProxy Key = BuildStaticMeshCollisionDerivedDataKey(
				KeyInput, OutError);
			if (!Key.IsValid()) return false;
			FByteArray Bytes;
			AssetDerivedDataCache::FOperationDiagnostic LoadDiagnostic;
			FCollisionGeometryRef Geometry;
			AssetDerivedDataCache::FOperationDiagnostic StoreDiagnostic;
			bool bCacheHit = false;
			if (AssetDerivedDataCache::Load(Key,
				MaximumStaticMeshCollisionPayloadBytes, Bytes, LoadDiagnostic)
				== AssetDerivedDataCache::ELoadResult::Hit)
				bCacheHit = DecodeCollision(Bytes, Mode, Policy, Geometry, LoadDiagnostic.Message);
			if (!bCacheHit)
			{
				FStaticMeshCollisionRecipeProduct RecipeProduct;
				if (!Provider.BuildCollision({Positions, Indices, Mode, Policy},
					RecipeProduct, OutError) || !RecipeProduct.Geometry) return false;
				Geometry = std::move(RecipeProduct.Geometry);
				if (!EncodeCollision(Geometry, Policy, Bytes, OutError)) return false;
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
			OutProduct.Descriptor = Descriptor;
			OutProduct.CacheReadNanoseconds = LoadDiagnostic.DurationNanoseconds;
			OutProduct.CacheWriteNanoseconds = StoreDiagnostic.DurationNanoseconds;
			OutProduct.Diagnostic = AssetDerivedDataCache::CombineDiagnostics(
				LoadDiagnostic, StoreDiagnostic);
			return true;
		});
		if (Invocation.Status == EFeatureInvokeStatus::Invoked
			&& Invocation.Value.has_value() && *Invocation.Value)
		{
			OutError.clear();
			return true;
		}
		OutProduct = {};
		SetInvocationError(Invocation.Status, OutError);
		return false;
#endif
	}
}
