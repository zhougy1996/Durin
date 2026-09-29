#pragma once

#include <expected>

#if DURIN_WITH_EDITOR

#include "Asset/DerivedDataCacheKeyProxy.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshBuildTypes.h"
#include "DerivedDataBuildDefinition.h"

namespace Durin
{
	inline constexpr uint32 StaticMeshRenderOutputSchemaVersion = 2;
	inline constexpr std::string_view StaticMeshCacheBucket = "StaticMesh/Objects";


	ENGINE_API auto BuildStaticMeshReconciliationHash(
		std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
		float NormalizedSize) -> FXxHash128;
	ENGINE_API auto BuildStaticMeshReconciliationHash(
		std::span<const FStaticMeshBuildMaterialSlot> MaterialSlots, float NormalizedSize) -> FXxHash128;
	ENGINE_API auto MakeStaticMeshSessionDefinition(uint32 MaterialSlotCount)
		-> std::expected<DerivedData::FBuildDefinition, DerivedData::FBuildDefinitionError>;
	ENGINE_API auto GetStaticMeshBuildDescriptor(uint32 BuilderVersion, uint32 OutputVersion = StaticMeshRenderOutputSchemaVersion)
		-> DerivedData::FBuildFunctionDescriptor;
	// Canonical Engine-owned identity for one StaticMesh render-data value.
	struct FStaticMeshBuildKeyInput
	{
		FXxHash128 SourceHash;
		FXxHash128 ReconciliationHash;
		uint32 BuilderVersion = StaticMeshBuilderVersion;
		uint32 OutputSchemaVersion = StaticMeshRenderOutputSchemaVersion;
		uint32 MaterialSlotCount = 1;
		EAssetPayloadTargetPlatform TargetPlatform = EAssetPayloadTargetPlatform::Unknown;

	};

	ENGINE_API auto MakeStaticMeshBuildAction(const FStaticMeshBuildKeyInput& Input)
		-> std::expected<DerivedData::FBuildAction, FStaticMeshBuildKeyError>;
	ENGINE_API auto FormatStaticMeshBuildKeyError(const FStaticMeshBuildKeyError& Error) -> std::string;
	ENGINE_API auto BuildStaticMeshDerivedDataKeyBytes(const FStaticMeshBuildKeyInput& Input) -> std::expected<FByteBuffer, FStaticMeshBuildKeyError>;
	ENGINE_API auto BuildStaticMeshDerivedDataKey(const FStaticMeshBuildKeyInput& Input) -> std::expected<FCacheKeyProxy, FStaticMeshBuildKeyError>;
}

#endif
