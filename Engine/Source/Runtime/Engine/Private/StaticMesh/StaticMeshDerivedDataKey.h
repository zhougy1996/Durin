#pragma once

#include "CoreMinimal.h"

#if DURIN_WITH_EDITOR

#include "Asset/DerivedDataCacheKeyProxy.h"
#include "Asset/PayloadTargetPlatform.h"
#include "Materials/MeshMaterialSlot.h"
#include "StaticMesh/StaticMeshBuildTypes.h"
#include "DerivedDataBuildDefinition.h"

namespace Durin
{
	inline constexpr uint32 StaticMeshRenderBuildFunctionVersion = 1;
	inline constexpr uint32 StaticMeshRenderOutputSchemaVersion = 2;

	ENGINE_API auto BuildStaticMeshReconciliationHash(
		std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
		float NormalizedSize) -> FXxHash128;
	ENGINE_API auto BuildStaticMeshReconciliationHash(
		std::span<const FStaticMeshBuildMaterialSlot> MaterialSlots, float NormalizedSize) -> FXxHash128;
	ENGINE_API auto MakeStaticMeshSessionDefinition(uint32 MaterialSlotCount, uint64 BuilderVersion)
		-> std::expected<DerivedData::FBuildDefinition, DerivedData::FBuildDefinitionError>;
	ENGINE_API auto GetStaticMeshBuildDescriptor(uint32 FunctionVersion = StaticMeshRenderBuildFunctionVersion, uint32 OutputVersion = StaticMeshRenderOutputSchemaVersion)
		-> DerivedData::FBuildFunctionDescriptor;
	enum class EArchiveFailureCode : uint8;
	enum class EStaticMeshBuildKeyError : uint8 { None, UnsupportedTarget, Archive };
	struct FStaticMeshBuildKeyError
	{
		EStaticMeshBuildKeyError Code = EStaticMeshBuildKeyError::None;
		EAssetPayloadTargetPlatform TargetPlatform = EAssetPayloadTargetPlatform::Unknown;
		std::optional<EArchiveFailureCode> ArchiveCode;
	};

	// Canonical Engine-owned identity for one StaticMesh render-data value.
	struct FStaticMeshBuildKeyInput
	{
		FXxHash128 SourceHash;
		FXxHash128 ReconciliationHash;
		uint64 BuilderVersion = 0;
		uint32 FunctionVersion = StaticMeshRenderBuildFunctionVersion;
		uint32 OutputSchemaVersion = StaticMeshRenderOutputSchemaVersion;
		uint32 MaterialSlotCount = 1;
		EAssetPayloadTargetPlatform TargetPlatform = EAssetPayloadTargetPlatform::Unknown;

	};

	ENGINE_API auto MakeStaticMeshBuildAction(const FStaticMeshBuildKeyInput& Input)
		-> std::expected<DerivedData::FBuildAction, FStaticMeshBuildKeyError>;
	ENGINE_API auto BuildStaticMeshDerivedDataKeyBytes(const FStaticMeshBuildKeyInput& Input) -> std::expected<FByteBuffer, FStaticMeshBuildKeyError>;
	ENGINE_API auto BuildStaticMeshDerivedDataKey(const FStaticMeshBuildKeyInput& Input) -> std::expected<FCacheKeyProxy, FStaticMeshBuildKeyError>;
}

#endif
