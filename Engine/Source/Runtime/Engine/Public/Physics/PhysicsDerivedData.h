#pragma once

#include <expected>
#include "EngineAPI.h"
#include "Asset/PayloadTargetPlatform.h"
#include "Hash/XxHash.h"
#include "Misc/Guid.h"
#include "Physics/BodySetupTypes.h"
#include "Physics/PhysicsCookVersion.h"
#include "Collision/CollisionGeometry.h"

namespace Durin
{
	class FArchive;
	inline constexpr uint32 PhysicsCollisionPayloadSchemaVersion = 2;
	inline constexpr uint32 PhysicsCollisionPayloadAlignment = 16;
	inline constexpr uint32 PhysicsCollisionPayloadHeaderSize = 64;
	inline constexpr uint32 PhysicsCollisionPayloadChunkEntrySize = 32;
	inline constexpr uint32 MaximumPhysicsCollisionPayloadChunks = 8;
	inline constexpr uint32 MaximumPhysicsCollisionVertices = 100'000'000;
	inline constexpr uint64 MaximumPhysicsCollisionPayloadBytes = 256ull * 1024ull * 1024ull;
	inline const FGuid PhysicsCollisionCookedPayloadId{
		0x3c10f7d1, 0x92fa4e20, 0xb544ad79, 0x1d788064};


	struct FPhysicsCollisionPayloadData
	{
		EBodySetupCollisionSourceMode SourceMode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionQueryPolicy QueryPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		std::vector<FVector3f> Positions;
		std::vector<uint32> Indices;
		std::vector<uint32> SourceOrdinals;
		std::vector<FCollisionGeometryNode> Nodes;
		std::vector<uint32> LeafTriangles;

		// Loads in place; discard failures. The byte owner checks completion before publication.
		ENGINE_API auto Serialize(
			FArchive& Ar,
			const std::function<bool()>& ShouldCancel = {}) -> void;
	};

	enum class EPhysicsCollisionPayloadOperation : uint8 { Extract, Construct };
	enum class EPhysicsCollisionPayloadError : uint8
	{
		None, InvalidGeometry, InvalidVertex, FloatStorage, InvalidTriangle, InvalidNode,
		InvalidMembership, InconsistentCounts, NonFinitePosition, UnexpectedBvh,
		DuplicateOrdinal, UnknownOrdinal, InvalidSourceMode, InvalidTopology, Cancelled,
	};
	struct FPhysicsCollisionPayloadError
	{
		EPhysicsCollisionPayloadError Code = EPhysicsCollisionPayloadError::None;
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

	ENGINE_API auto MakePhysicsCollisionPayloadData(
		const FCollisionGeometryRef& Geometry,
		EBodySetupCollisionQueryPolicy QueryPolicy,
		FPhysicsCollisionPayloadData& OutPayload,
		const std::function<bool()>& ShouldCancel = {}) -> std::expected<void, FPhysicsCollisionPayloadError>;
	ENGINE_API auto MakePhysicsCollisionGeometry(
		const FPhysicsCollisionPayloadData& Payload,
		FCollisionGeometryRef& OutGeometry,
		const std::function<bool()>& ShouldCancel = {}) -> std::expected<void, FPhysicsCollisionPayloadError>;

	enum class EArchiveFailureCode : uint8;
	enum class EPhysicsCacheCodecError : uint8 { None, CollisionPayload, Archive, CollisionMetadata };
	enum class EPhysicsCacheCodecOperation : uint8 { EncodeCollision, DecodeCollision };
	struct FPhysicsCacheCodecError
	{
		EPhysicsCacheCodecError Code = EPhysicsCacheCodecError::None;
		EPhysicsCacheCodecOperation Operation = EPhysicsCacheCodecOperation::EncodeCollision;
		uint64 Actual = 0;
		uint64 Expected = 0;
		std::optional<EArchiveFailureCode> ArchiveCode;
		std::string ArchivePath;
		EBodySetupCollisionSourceMode ActualMode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionSourceMode ExpectedMode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionQueryPolicy ActualPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		EBodySetupCollisionQueryPolicy ExpectedPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		std::optional<FPhysicsCollisionPayloadError> CollisionCause;
	};

	ENGINE_API auto FormatPhysicsCacheCodecError(const FPhysicsCacheCodecError& Error) -> std::string;


}
