#pragma once

#include "CoreMinimal.h"

#if DURIN_WITH_EDITOR
#include "Physics/PhysicsDerivedData.h"
#include "DerivedDataBuildDefinition.h"

namespace Durin
{
	inline constexpr uint32 PhysicsCollisionOutputSchemaVersion = 3;
	inline constexpr uint32 PhysicsCollisionBuildFunctionVersion = 3;
	inline constexpr uint32 PhysicsCollisionConstantsSchemaVersion = 3;
	// Bucket identity is stable; definition keys intentionally invalidate old entries.
	inline constexpr std::string_view PhysicsCollisionCacheBucket =
		"StaticMeshCollision";
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
		uint32 WeldToleranceBits = 0;
		uint64 BuilderVersion = 0;
		uint32 FunctionVersion = PhysicsCollisionBuildFunctionVersion;
		uint32 OutputSchemaVersion = PhysicsCollisionOutputSchemaVersion;
		EAssetPayloadTargetPlatform TargetPlatform = EAssetPayloadTargetPlatform::Unknown;

	};

	ENGINE_API auto MakePhysicsCookSessionDefinition(EBodySetupCollisionSourceMode Mode,
		uint64 BuilderVersion, uint32 WeldToleranceBits = 0,
		EAssetPayloadTargetPlatform TargetPlatform = EAssetPayloadTargetPlatform::Win64)
		-> std::expected<DerivedData::FBuildDefinition, DerivedData::FBuildDefinitionError>;
	ENGINE_API auto GetPhysicsCookBuildDescriptor(uint32 FunctionVersion = PhysicsCollisionBuildFunctionVersion,
		uint32 OutputVersion = PhysicsCollisionOutputSchemaVersion) -> DerivedData::FBuildFunctionDescriptor;

	ENGINE_API auto MakePhysicsCookBuildAction(const FPhysicsCookKeyInput& Input)
		-> std::expected<DerivedData::FBuildAction, FPhysicsCookKeyError>;
}
#endif
