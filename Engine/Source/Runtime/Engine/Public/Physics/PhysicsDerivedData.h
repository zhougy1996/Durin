#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "Asset/PayloadTargetPlatform.h"
#include "Misc/Guid.h"
#include "Physics/BodySetupTypes.h"
#include "Collision/CollisionGeometry.h"

namespace Durin
{
	class FArchive;
	inline constexpr uint32 PhysicsCollisionPayloadSchemaVersion = 3;
	inline constexpr uint32 PhysicsCollisionPayloadAlignment = 16;
	inline constexpr uint32 PhysicsCollisionPayloadHeaderSize = 60;
	inline constexpr uint32 PhysicsCollisionPayloadChunkEntrySize = 32;
	inline constexpr uint32 MaximumPhysicsCollisionPayloadChunks = 8;
	inline constexpr uint32 MaximumPhysicsCollisionVertices = 100'000'000;
	inline constexpr uint64 MaximumPhysicsCollisionPayloadBytes = 256ull * 1024ull * 1024ull;
	inline const FGuid PhysicsCollisionCookedPayloadId{
		0x3c10f7d1, 0x92fa4e20, 0xb544ad79, 0x1d788064};


	struct FPhysicsCollisionPayloadData
	{
		EBodySetupCollisionSourceMode SourceMode = EBodySetupCollisionSourceMode::None;
		std::vector<FVector3f> Positions;
		std::vector<uint32> Indices;
		std::vector<uint32> SourceOrdinals;
		std::vector<FCollisionGeometryNode> Nodes;
		std::vector<uint32> LeafTriangles;

		// Loads in place; discard failures. The byte owner checks completion before publication.
		ENGINE_API auto Serialize(FArchive& Ar) -> void;
	};

	// Synchronous conversions commit complete products only; failure leaves outputs unchanged.
	ENGINE_API auto MakePhysicsCollisionPayloadData(const FCollisionGeometryRef& Geometry, FPhysicsCollisionPayloadData& OutPayload) -> bool;
	ENGINE_API auto MakePhysicsCollisionGeometry(const FPhysicsCollisionPayloadData& Payload, FCollisionGeometryRef& OutGeometry) -> bool;
}
