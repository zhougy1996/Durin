#pragma once

#include "AssetBuild/BuildFunction.h"
#include "Skeletal/SkeletalBuildOperations.h"

namespace Durin::Asset::Build::Private
{
	extern const FBuildFunctionIdentity SkeletalMeshFunctionIdentity;
	extern const FBuildFunctionIdentity AnimationClipFunctionIdentity;
	inline constexpr std::string_view SkeletalMeshInputName = "SkeletalMeshBuildInput";
	inline constexpr std::string_view AnimationClipInputName = "AnimationClipBuildInput";
	inline constexpr std::string_view SkeletalValueName = "SkeletalPayload";

	auto EncodeSkeletalMeshPayload(
		FSkeletalMeshPayloadData& Payload,
		const FSkeletalPayloadSerializationContext& Context,
		std::vector<std::byte>& OutBytes,
		std::string& OutError) -> bool;
	auto EncodeAnimationClipPayload(
		FAnimationClipPayloadData& Payload,
		const FSkeletalPayloadSerializationContext& Context,
		std::vector<std::byte>& OutBytes,
		std::string& OutError) -> bool;
	auto DecodeSkeletalMeshPayload(
		const FBuildValue& Value, std::string_view Key,
		const FSkeletalPayloadSerializationContext& Context,
		FSkeletalMeshPayloadData& OutPayload,
		std::string& OutError) -> bool;
	auto DecodeAnimationClipPayload(
		const FBuildValue& Value, std::string_view Key,
		const FSkeletalPayloadSerializationContext& Context,
		FAnimationClipPayloadData& OutPayload,
		std::string& OutError) -> bool;

	auto CreateSkeletalMeshBuildFunction() -> std::shared_ptr<IBuildFunction>;
	auto CreateAnimationClipBuildFunction() -> std::shared_ptr<IBuildFunction>;
}
