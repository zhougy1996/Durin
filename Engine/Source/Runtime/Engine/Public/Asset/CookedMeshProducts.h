#pragma once

#include "CoreMinimal.h"
#include "Collision/CollisionGeometry.h"

#include "EngineAPI.h"
#include "Materials/MeshMaterialSlot.h"
#include "Physics/BodySetupTypes.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Asset/CookedAsset.h"

namespace Durin
{
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
		FStaticMeshCookedProduct& OutProduct,
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64
	) -> bool;
}
