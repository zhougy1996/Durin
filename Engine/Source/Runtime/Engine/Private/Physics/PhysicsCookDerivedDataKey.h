#pragma once

#if DURIN_WITH_EDITOR
#include "Physics/PhysicsDerivedData.h"
#include "Asset/DerivedDataCacheKeyProxy.h"

namespace Durin
{
	inline constexpr uint32 PhysicsCollisionKeySchemaVersion = 3;
	// Historical bucket identity is retained for byte-compatible existing caches.
	inline constexpr std::string_view PhysicsCollisionCacheBucket =
		"StaticMeshCollision/Objects";
	enum class EArchiveFailureCode : uint8;
	enum class EPhysicsCookKeyError : uint8 { None, UnsupportedTarget, Archive };
	struct FPhysicsCookKeyError
	{
		EPhysicsCookKeyError Code = EPhysicsCookKeyError::None;
		EAssetPayloadTargetPlatform TargetPlatform = EAssetPayloadTargetPlatform::Unknown;
		std::optional<EArchiveFailureCode> ArchiveCode;
		std::string ArchivePath;
	};

	// Canonical Engine-owned identity for one physics collision value.
	struct FPhysicsCookKeyInput
	{
		FXxHash128 GeometryHash;
		EBodySetupCollisionSourceMode SourceMode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionQueryPolicy QueryPolicy =
			EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		uint32 WeldToleranceBits = 0;
		uint32 BuilderVersion = PhysicsCookBuilderVersion;
		uint32 PayloadSchemaVersion = PhysicsCollisionPayloadSchemaVersion;
		EAssetPayloadTargetPlatform TargetPlatform = EAssetPayloadTargetPlatform::Unknown;

		ENGINE_API auto Serialize(FArchive& Ar) -> void;
	};

	ENGINE_API auto FormatPhysicsCookKeyError(const FPhysicsCookKeyError& Error) -> std::string;
	ENGINE_API auto BuildPhysicsCookDerivedDataKeyBytes(const FPhysicsCookKeyInput& Input) -> std::expected<FByteBuffer, FPhysicsCookKeyError>;
	ENGINE_API auto BuildPhysicsCookDerivedDataKey(const FPhysicsCookKeyInput& Input) -> std::expected<FCacheKeyProxy, FPhysicsCookKeyError>;
}
#endif
