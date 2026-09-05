#pragma once

#if DURIN_WITH_EDITOR

#include "DerivedDataCacheKeyProxy.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace Durin
{
	inline constexpr uint32 StaticMeshDerivedDataKeySchemaVersion = 4;
	inline constexpr uint32 StaticMeshCollisionKeySchemaVersion = 3;
	inline constexpr std::string_view StaticMeshCacheBucket = "StaticMesh/Objects";
	inline constexpr std::string_view StaticMeshCollisionCacheBucket =
		"StaticMeshCollision/Objects";

	ENGINE_API auto BuildStaticMeshReconciliationHash(
		std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
		float NormalizedSize) -> FXxHash128;
	// Canonical Engine-owned identity for one StaticMesh render-data value.
	struct FStaticMeshBuildKeyInput
	{
		FXxHash128 ImportedDataHash;
		FXxHash128 ReconciliationHash;
		uint32 BuilderVersion = StaticMeshBuilderVersion;
		uint32 PayloadSchemaVersion = StaticMeshPayloadSchemaVersion;
		EStaticMeshTargetPlatform TargetPlatform = EStaticMeshTargetPlatform::Unknown;

		ENGINE_API auto Serialize(FArchive& Ar) -> void;
	};

	// Canonical Engine-owned identity for one StaticMesh collision value.
	struct FStaticMeshCollisionBuildKeyInput
	{
		FXxHash128 GeometryHash;
		EBodySetupCollisionSourceMode SourceMode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionQueryPolicy QueryPolicy =
			EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		uint32 WeldToleranceBits = 0;
		uint32 BuilderVersion = StaticMeshCollisionBuilderVersion;
		uint32 PayloadSchemaVersion = StaticMeshCollisionPayloadSchemaVersion;
		EStaticMeshTargetPlatform TargetPlatform = EStaticMeshTargetPlatform::Unknown;

		ENGINE_API auto Serialize(FArchive& Ar) -> void;
	};

	ENGINE_API auto BuildStaticMeshDerivedDataKeyBytes(
		const FStaticMeshBuildKeyInput& Input,
		std::string& OutError) -> FByteBuffer;
	ENGINE_API auto BuildStaticMeshDerivedDataKey(
		const FStaticMeshBuildKeyInput& Input,
		std::string& OutError) -> FCacheKeyProxy;
	ENGINE_API auto BuildStaticMeshCollisionDerivedDataKeyBytes(
		const FStaticMeshCollisionBuildKeyInput& Input,
		std::string& OutError) -> FByteBuffer;
	ENGINE_API auto BuildStaticMeshCollisionDerivedDataKey(
		const FStaticMeshCollisionBuildKeyInput& Input,
		std::string& OutError) -> FCacheKeyProxy;

}

#endif
