#pragma once

#include "EngineAPI.h"
#include "Materials/MeshMaterialSlot.h"
#include "Physics/BodySetup.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace Durin
{
	enum class ECookedMeshProductFailure : uint8
	{
		None,
		Schema,
		Metadata,
		Construction,
	};

	struct FCookedMeshProductError
	{
		ECookedMeshProductFailure Category = ECookedMeshProductFailure::None;
		std::string Message;

		explicit operator bool() const { return Category != ECookedMeshProductFailure::None; }
	};

	struct FStaticMeshCookedProduct
	{
		std::unique_ptr<FStaticMeshRenderData> RenderData;
		FCollisionGeometryRef SimpleCollision;
		FCollisionGeometryRef ComplexCollision;
		uint64 CollisionPayloadBytes = 0;
		bool bHasCollision = false;
	};

	struct FSkeletalMeshCookedProduct
	{
		std::shared_ptr<const FSkeletalMeshPayloadData> Payload;
		std::unique_ptr<FSkeletalMeshRenderData> RenderData;
	};

	// Worker-safe codecs. Every input is detached from managed objects and every
	// output owns the CPU state required for later GameThread publication.
	ENGINE_API auto DecodeStaticMeshCookedProduct(
		std::span<const std::byte> RenderBytes,
		std::span<const std::byte> CollisionBytes,
		std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
		EBodySetupCollisionSourceMode CollisionMode,
		EBodySetupCollisionQueryPolicy CollisionPolicy,
		FStaticMeshCookedProduct& OutProduct,
		FCookedMeshProductError& OutError) -> bool;

	ENGINE_API auto DecodeSkeletalMeshCookedProduct(
		std::span<const std::byte> Bytes,
		std::span<const FSkeletonBone> SkeletonBones,
		const FSkeletonTransform& MeshNodeBindTransform,
		std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
		const FSkeletalMeshSummary& ExpectedSummary,
		FSkeletalMeshCookedProduct& OutProduct,
		FCookedMeshProductError& OutError) -> bool;
}
