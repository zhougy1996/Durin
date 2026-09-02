#pragma once

#if DURIN_WITH_EDITOR

#include "SkeletalMesh/SkeletalDerivedData.h"

namespace Durin
{
	inline constexpr uint32 SkeletalPayloadKeySchemaVersion = 4;
	// Carries the source-independent identity of a skeletal derived value.
	struct FSkeletalBuildKeyFields
	{
		std::string ProviderIdentity;
		uint32 ProviderVersion = 0;
		FXxHash128 ImportedDataIdentity;
		FXxHash128 PayloadInputFingerprint;
		std::string SkeletonCompatibilityIdentity;
		ESkeletalPayloadTargetPlatform TargetPlatform =
			ESkeletalPayloadTargetPlatform::Unknown;
		ESkeletalPayloadTargetProfile TargetProfile =
			ESkeletalPayloadTargetProfile::Unknown;
	};

	struct FSkeletalMeshBuildKeyInput : FSkeletalBuildKeyFields
	{
		ENGINE_API auto Serialize(FArchive& Ar) -> void;
	};

	struct FAnimationClipBuildKeyInput : FSkeletalBuildKeyFields
	{
		ENGINE_API auto Serialize(FArchive& Ar) -> void;
	};

	ENGINE_API auto BuildSkeletalMeshDerivedDataKeyBytes(
		const FSkeletalMeshBuildKeyInput& Input,
		std::string& OutError) -> FByteArray;
	ENGINE_API auto BuildSkeletalMeshDerivedDataKey(
		const FSkeletalMeshBuildKeyInput& Input,
		std::string& OutError) -> std::string;
	ENGINE_API auto BuildAnimationClipDerivedDataKeyBytes(
		const FAnimationClipBuildKeyInput& Input,
		std::string& OutError) -> FByteArray;
	ENGINE_API auto BuildAnimationClipDerivedDataKey(
		const FAnimationClipBuildKeyInput& Input,
		std::string& OutError) -> std::string;

}

#endif
