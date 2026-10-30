#pragma once

#include "CoreMinimal.h"
#include "Asset/CookedMeshProducts.h"
#include "Physics/PhysicsPayloadPrivate.h"
#include "StaticMesh/StaticMeshPayloadPrivate.h"
#include "Serialization/Archive.h"

namespace Durin::AssetPrivate
{
	enum class ECookedMeshProductError : uint8
	{
		MissingCollision,
		CollisionArchive,
		CollisionMetadata,
		CollisionConstruction,
		RenderArchive,
		MaterialSlotCount,
		RenderConstruction,
		RuntimeMaterialSlotCount
	};

	struct FCookedMeshProductError
	{
		ECookedMeshProductError Code = ECookedMeshProductError::MissingCollision;
		std::optional<EArchiveFailureCode> ArchiveCode;
		std::string ArchivePath;
		uint64 ByteOffset = 0;
		uint64 ByteCount = 0;
		uint64 Actual = 0;
		uint64 Expected = 0;
		EBodySetupCollisionSourceMode ActualMode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionSourceMode ExpectedMode = EBodySetupCollisionSourceMode::None;
		std::optional<PhysicsPrivate::FPhysicsCollisionPayloadError> CollisionCause;
		std::optional<StaticMeshPrivate::FStaticMeshPayloadError> RenderCause;
	};

	ENGINE_API auto FormatCookedMeshProductError(const FCookedMeshProductError& Error) -> std::string;
	ENGINE_API auto TryDecodeStaticMeshCookedProduct(FByteView RenderBytes, FByteView CollisionBytes, std::span<const FMeshMaterialSlotDefinition> MaterialSlots, EBodySetupCollisionSourceMode CollisionMode, FStaticMeshCookedProduct& OutProduct, ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64)
		-> std::expected<void, FCookedMeshProductError>;
} // namespace Durin::AssetPrivate
