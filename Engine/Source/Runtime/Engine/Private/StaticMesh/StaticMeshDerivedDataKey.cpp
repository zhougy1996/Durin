#include "StaticMesh/StaticMeshDerivedDataKey.h"

#if DURIN_WITH_EDITOR

#include "DerivedDataBuildDefinition.h"
#include "StaticMesh/StaticMeshSource.h"

#include "Serialization/Archive.h"
#include "Hash/CanonicalHash.h"

namespace Durin
{
	auto BuildStaticMeshReconciliationHash(
		std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
		float NormalizedSize) -> FXxHash128
	{
		FXxHash128Builder Builder;
		UpdateCanonicalHash(Builder, std::bit_cast<uint32>(NormalizedSize));
		UpdateCanonicalHash(Builder, static_cast<uint64>(MaterialSlots.size()));
		for (const FMeshMaterialSlotDefinition& Slot : MaterialSlots)
		{
			UpdateCanonicalHashString(Builder, Slot.Name.ToString());
			UpdateCanonicalHashString(Builder, Slot.SourceName);
			UpdateCanonicalHash(Builder, Slot.SourceMaterialIndex);
		}
		return Builder.Finalize();
	}
	auto FormatStaticMeshBuildKeyError(const FStaticMeshBuildKeyError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EStaticMeshBuildKeyError::None: return {};
		case EStaticMeshBuildKeyError::UnsupportedTarget:
			return std::format("StaticMesh derived-data target {} is unsupported.", static_cast<uint32>(Error.TargetPlatform));
		case EStaticMeshBuildKeyError::Archive:
			return std::format("StaticMesh derived-data key encoding failed (Archive code {}, path '{}').",
				Error.ArchiveCode ? static_cast<int>(*Error.ArchiveCode) : -1, Error.ArchivePath);
		}
		return {};
	}

	auto MakeStaticMeshBuildDefinition(const FStaticMeshBuildKeyInput& Input)
		-> std::expected<DerivedData::FBuildDefinition, FStaticMeshBuildKeyError>
	{
		using namespace DerivedData;
		if (Input.TargetPlatform != EAssetPayloadTargetPlatform::Win64)
			return std::unexpected(FStaticMeshBuildKeyError{.Code = EStaticMeshBuildKeyError::UnsupportedTarget, .TargetPlatform = Input.TargetPlatform});
		auto Definition = FBuildDefinition::TryCreate({"Durin.StaticMesh.Render", Input.BuilderVersion, 1,
			"StaticMesh.RenderData", Input.PayloadSchemaVersion, FCacheBucket::FromString(StaticMeshCacheBucket)},
			{{"TargetPlatform", uint64(Input.TargetPlatform)}, {"Reconciliation", Input.ReconciliationHash}},
			{{"Source", Input.SourceHash, "StaticMeshSource", StaticMeshSourceGeometryPayloadVersion, "StaticMesh.Geometry", 1}});
		if (!Definition) return std::unexpected(FStaticMeshBuildKeyError{.Code = EStaticMeshBuildKeyError::Archive,
			.TargetPlatform = Input.TargetPlatform, .ArchiveCode = EArchiveFailureCode::InvalidData});
		return std::move(*Definition);
	}

	auto BuildStaticMeshDerivedDataKeyBytes(const FStaticMeshBuildKeyInput& Input) -> std::expected<FByteBuffer, FStaticMeshBuildKeyError>
	{
		auto Definition = MakeStaticMeshBuildDefinition(Input);
		if (!Definition) return std::unexpected(Definition.error());
		return FByteBuffer(Definition->GetCanonicalBytes().begin(), Definition->GetCanonicalBytes().end());
	}
	auto BuildStaticMeshDerivedDataKey(const FStaticMeshBuildKeyInput& Input) -> std::expected<FCacheKeyProxy, FStaticMeshBuildKeyError>
	{
		auto Definition = MakeStaticMeshBuildDefinition(Input);
		if (!Definition) return std::unexpected(Definition.error());
		return FCacheKeyProxy(Definition->GetKey());
	}
}
#endif
