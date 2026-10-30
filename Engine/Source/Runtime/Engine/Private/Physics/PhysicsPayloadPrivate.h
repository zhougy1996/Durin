#pragma once

#include "CoreMinimal.h"
#include "Physics/PhysicsDerivedData.h"

namespace Durin::PhysicsPrivate
{
	enum class EPhysicsCollisionPayloadOperation : uint8
	{
		Extract,
		Construct
	};
	enum class EPhysicsCollisionPayloadError : uint8
	{
		InvalidGeometry,
		InvalidVertex,
		FloatStorage,
		InvalidTriangle,
		InvalidNode,
		InvalidMembership,
		InconsistentCounts,
		NonFinitePosition,
		UnexpectedBvh,
		DuplicateOrdinal,
		UnknownOrdinal,
		InvalidSourceMode,
		InvalidTopology,
	};
	struct FPhysicsCollisionPayloadError
	{
		EPhysicsCollisionPayloadError Code = EPhysicsCollisionPayloadError::InvalidGeometry;
		EPhysicsCollisionPayloadOperation Operation = EPhysicsCollisionPayloadOperation::Construct;
		EBodySetupCollisionSourceMode SourceMode = EBodySetupCollisionSourceMode::None;
		std::optional<ECollisionGeometryKind> GeometryKind;
		uint64 VertexCount = 0;
		uint64 IndexCount = 0;
		uint64 OrdinalCount = 0;
		uint64 NodeCount = 0;
		uint64 LeafCount = 0;
		uint64 Index = 0;
		uint32 Ordinal = 0;
		FVector3 Position = FVector3(0);
	};

	ENGINE_API auto FormatPhysicsCollisionPayloadError(const FPhysicsCollisionPayloadError& Error) -> std::string;
	ENGINE_API auto TryMakePhysicsCollisionPayloadData(const FCollisionGeometryRef& Geometry, FPhysicsCollisionPayloadData& OutPayload) -> std::expected<void, FPhysicsCollisionPayloadError>;
	ENGINE_API auto TryMakePhysicsCollisionGeometry(const FPhysicsCollisionPayloadData& Payload, FCollisionGeometryRef& OutGeometry) -> std::expected<void, FPhysicsCollisionPayloadError>;
} // namespace Durin::PhysicsPrivate
