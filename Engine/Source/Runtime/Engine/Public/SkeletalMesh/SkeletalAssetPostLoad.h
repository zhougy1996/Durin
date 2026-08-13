#pragma once

#include "EngineAPI.h"

namespace Durin
{
	struct FAnimationClipPayloadData;
	struct FSkeletalMeshPayloadData;
	struct FSkeletalPayloadSerializationContext;

	using FSkeletalMeshUncookedPayloadLoader = std::function<bool(
		std::string_view,
		const FSkeletalPayloadSerializationContext&,
		FSkeletalMeshPayloadData&,
		std::string&)>;
	using FAnimationClipUncookedPayloadLoader = std::function<bool(
		std::string_view,
		const FSkeletalPayloadSerializationContext&,
		FAnimationClipPayloadData&,
		std::string&)>;

	// Installs authoring-only DDC policy without exposing the store to Runtime Engine.
	ENGINE_API auto RegisterSkeletalAssetUncookedPayloadLoaders(
		FSkeletalMeshUncookedPayloadLoader MeshLoader,
		FAnimationClipUncookedPayloadLoader ClipLoader) -> bool;
	ENGINE_API auto UnregisterSkeletalAssetUncookedPayloadLoaders() -> void;
	ENGINE_API auto InvokeSkeletalMeshUncookedPayloadLoader(
		std::string_view Key,
		const FSkeletalPayloadSerializationContext& Context,
		FSkeletalMeshPayloadData& OutPayload,
		std::string& OutMessage) -> bool;
	ENGINE_API auto InvokeAnimationClipUncookedPayloadLoader(
		std::string_view Key,
		const FSkeletalPayloadSerializationContext& Context,
		FAnimationClipPayloadData& OutPayload,
		std::string& OutMessage) -> bool;
}
