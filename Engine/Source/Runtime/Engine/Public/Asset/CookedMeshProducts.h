#pragma once
#include "Physics/PhysicsDerivedData.h"

#include "EngineAPI.h"
#include "Materials/MeshMaterialSlot.h"
#include "Physics/BodySetup.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace Durin
{
	enum class EArchiveFailureCode : uint8;
	enum class ECookedMeshProductError : uint8
	{
		None, MissingCollision, CollisionArchive, CollisionMetadata,
		CollisionConstruction, RenderArchive, MaterialSlotCount,
		RenderConstruction, RuntimeMaterialSlotCount
	};

	struct FCookedMeshProductError
	{
		ECookedMeshProductError Code = ECookedMeshProductError::None;
		std::optional<EArchiveFailureCode> ArchiveCode;
		std::string ArchivePath;
		uint64 ByteOffset = 0;
		uint64 ByteCount = 0;
		uint64 Actual = 0;
		uint64 Expected = 0;
		EBodySetupCollisionSourceMode ActualMode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionSourceMode ExpectedMode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionQueryPolicy ActualPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		EBodySetupCollisionQueryPolicy ExpectedPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		std::optional<FPhysicsCollisionPayloadError> CollisionCause;
		std::optional<FStaticMeshPayloadError> RenderCause;
	};

	struct FCookedMeshProductResult
	{
		FCookedMeshProductError Error;
		explicit operator bool() const { return Error.Code == ECookedMeshProductError::None; }
	};
	ENGINE_API auto FormatCookedMeshProductError(const FCookedMeshProductError& Error) -> std::string;

	struct FStaticMeshCookedProduct
	{
		std::unique_ptr<FStaticMeshRenderData> RenderData;
		FCollisionGeometryRef SimpleCollision;
		FCollisionGeometryRef ComplexCollision;
		uint64 CollisionPayloadBytes = 0;
		bool bHasCollision = false;
	};

	// Worker-safe codecs. Every input is detached from managed objects and every
	// output owns the CPU state required for later GameThread publication.
	ENGINE_API auto DecodeStaticMeshCookedProduct(
		FByteView RenderBytes,
		FByteView CollisionBytes,
		std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
		EBodySetupCollisionSourceMode CollisionMode,
		EBodySetupCollisionQueryPolicy CollisionPolicy,
		FStaticMeshCookedProduct& OutProduct) -> FCookedMeshProductResult;
}
