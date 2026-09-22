#pragma once

#include <expected>

#if DURIN_WITH_EDITOR

#include "Asset/DerivedDataCacheKeyProxy.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace Durin
{
	inline constexpr uint32 StaticMeshDerivedDataKeySchemaVersion = 4;
	inline constexpr std::string_view StaticMeshCacheBucket = "StaticMesh/Objects";


	ENGINE_API auto BuildStaticMeshReconciliationHash(
		std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
		float NormalizedSize) -> FXxHash128;
	// Canonical Engine-owned identity for one StaticMesh render-data value.
	struct FStaticMeshBuildKeyInput
	{
		FXxHash128 SourceHash;
		FXxHash128 ReconciliationHash;
		uint32 BuilderVersion = StaticMeshBuilderVersion;
		uint32 PayloadSchemaVersion = StaticMeshPayloadSchemaVersion;
		EAssetPayloadTargetPlatform TargetPlatform = EAssetPayloadTargetPlatform::Unknown;

		ENGINE_API auto Serialize(FArchive& Ar) -> void;
	};

	ENGINE_API auto FormatStaticMeshBuildKeyError(const FStaticMeshBuildKeyError& Error) -> std::string;
	ENGINE_API auto BuildStaticMeshDerivedDataKeyBytes(const FStaticMeshBuildKeyInput& Input) -> std::expected<FByteBuffer, FStaticMeshBuildKeyError>;
	ENGINE_API auto BuildStaticMeshDerivedDataKey(const FStaticMeshBuildKeyInput& Input) -> std::expected<FCacheKeyProxy, FStaticMeshBuildKeyError>;
}

#endif
