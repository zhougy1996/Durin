#pragma once

#if DURIN_WITH_EDITOR
#include "Physics/PhysicsDerivedData.h"
#include "Asset/DerivedDataCacheKeyProxy.h"
#include "DerivedDataBuildDefinition.h"

namespace Durin
{
	inline constexpr uint32 PhysicsCollisionOutputSchemaVersion = 2;
	// Bucket identity is stable; definition keys intentionally invalidate old entries.
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
		uint32 OutputSchemaVersion = PhysicsCollisionOutputSchemaVersion;
		EAssetPayloadTargetPlatform TargetPlatform = EAssetPayloadTargetPlatform::Unknown;

	};

	ENGINE_API auto MakePhysicsCookSessionDefinition(EBodySetupCollisionSourceMode Mode,
		EBodySetupCollisionQueryPolicy Policy, uint32 WeldToleranceBits = 0)
		-> std::expected<DerivedData::FBuildDefinition, DerivedData::FBuildDefinitionError>;
	ENGINE_API auto GetPhysicsCookBuildDescriptor(uint32 BuilderVersion = PhysicsCookBuilderVersion,
		uint32 OutputVersion = PhysicsCollisionOutputSchemaVersion) -> DerivedData::FBuildFunctionDescriptor;

	ENGINE_API auto MakePhysicsCookBuildAction(const FPhysicsCookKeyInput& Input)
		-> std::expected<DerivedData::FBuildAction, FPhysicsCookKeyError>;
	ENGINE_API auto FormatPhysicsCookKeyError(const FPhysicsCookKeyError& Error) -> std::string;
	ENGINE_API auto BuildPhysicsCookDerivedDataKeyBytes(const FPhysicsCookKeyInput& Input) -> std::expected<FByteBuffer, FPhysicsCookKeyError>;
	ENGINE_API auto BuildPhysicsCookDerivedDataKey(const FPhysicsCookKeyInput& Input) -> std::expected<FCacheKeyProxy, FPhysicsCookKeyError>;
}
#endif
