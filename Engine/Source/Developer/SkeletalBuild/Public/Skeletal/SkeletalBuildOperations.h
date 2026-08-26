#pragma once

#include "SkeletalBuildAPI.h"
#include "Animation/AnimationClip.h"
#include "SkeletalMesh/SkeletalDerivedData.h"
#include "SkeletalMesh/SkeletalMesh.h"

namespace Durin::Asset
{
	inline constexpr uint32 SkeletalPayloadKeySchemaVersion = 1;
	inline constexpr uint32 SkeletalMeshBuilderVersion = 1;
	inline constexpr uint32 AnimationClipBuilderVersion = 1;
	inline constexpr std::string_view SkeletalMeshBuilderIdentity =
		"Durin.SkeletalMesh.Builder.V1";
	inline constexpr std::string_view AnimationClipBuilderIdentity =
		"Durin.AnimationClip.Builder.V1";

	// Carries source-independent identity shared by skeletal Build recipes.
	struct FSkeletalBuildKeyFields
	{
		std::string ProviderIdentity;
		uint32 ProviderVersion = 0;
		FXxHash128 SourceClosureHash;
		FXxHash128 SettingsHash;
		FXxHash128 ProviderStateHash;
		FXxHash128 PayloadInputFingerprint;
		std::string StableOutputIdentity;
		std::string SkeletonCompatibilityIdentity;
		ESkeletalPayloadTargetPlatform TargetPlatform =
			ESkeletalPayloadTargetPlatform::Unknown;
		ESkeletalPayloadTargetProfile TargetProfile =
			ESkeletalPayloadTargetProfile::Unknown;
	};

	struct FSkeletalMeshBuildKeyInput : FSkeletalBuildKeyFields
	{
		SKELETALBUILD_API auto Serialize(FArchive& Ar) -> void;
	};

	struct FAnimationClipBuildKeyInput : FSkeletalBuildKeyFields
	{
		SKELETALBUILD_API auto Serialize(FArchive& Ar) -> void;
	};

	// Owned normalized geometry and relationship facts consumed by the Build worker.
	struct FSkeletalMeshBuildRequest
	{
		uint32 SkeletonBoneCount = 0;
		std::string SkeletonCompatibilityIdentity;
		FSkeletonTransform MeshNodeBindTransform;
		uint32 MaterialSlotCount = 0;
		std::shared_ptr<const FSkeletalMeshPayloadData> Payload;
		FSkeletalMeshBuildKeyInput KeyInput;
	};

	// Owned normalized track data and relationship facts consumed by the Build worker.
	struct FAnimationClipBuildRequest
	{
		uint32 SkeletonBoneCount = 0;
		std::string SkeletonCompatibilityIdentity;
		FName ClipName;
		std::shared_ptr<const FAnimationClipPayloadData> Payload;
		FAnimationClipBuildKeyInput KeyInput;
	};

	struct FSkeletalMeshBuildProduct
	{
		FSkeletonTransform MeshNodeBindTransform;
		std::shared_ptr<const FSkeletalMeshPayloadData> Payload;
		std::string SkeletonCompatibilityIdentity;
		std::string DerivedDataKey;
		std::string Diagnostic;
	};

	struct FAnimationClipBuildProduct
	{
		FName ClipName;
		std::shared_ptr<const FAnimationClipPayloadData> Payload;
		std::string SkeletonCompatibilityIdentity;
		std::string DerivedDataKey;
		std::string Diagnostic;
	};

	SKELETALBUILD_API auto BuildSkeletalMeshDerivedDataKeyBytes(
		const FSkeletalMeshBuildKeyInput& Input,
		std::string& OutError) -> std::vector<std::byte>;
	SKELETALBUILD_API auto BuildSkeletalMeshDerivedDataKey(
		const FSkeletalMeshBuildKeyInput& Input,
		std::string& OutError) -> std::string;
	SKELETALBUILD_API auto BuildAnimationClipDerivedDataKeyBytes(
		const FAnimationClipBuildKeyInput& Input,
		std::string& OutError) -> std::vector<std::byte>;
	SKELETALBUILD_API auto BuildAnimationClipDerivedDataKey(
		const FAnimationClipBuildKeyInput& Input,
		std::string& OutError) -> std::string;

	SKELETALBUILD_API auto BuildSkeletalMeshProduct(
		FSkeletalMeshBuildRequest Request,
		FSkeletalMeshBuildProduct& OutProduct,
		std::string& OutError) -> bool;
	SKELETALBUILD_API auto BuildAnimationClipProduct(
		FAnimationClipBuildRequest Request,
		FAnimationClipBuildProduct& OutProduct,
		std::string& OutError) -> bool;

	SKELETALBUILD_API auto LoadSkeletalMeshDerivedData(
		std::string_view Key,
		const FSkeletalPayloadSerializationContext& Context,
		FSkeletalMeshPayloadData& OutPayload,
		std::string& OutMessage) -> bool;
	SKELETALBUILD_API auto LoadAnimationClipDerivedData(
		std::string_view Key,
		const FSkeletalPayloadSerializationContext& Context,
		FAnimationClipPayloadData& OutPayload,
		std::string& OutMessage) -> bool;
}
