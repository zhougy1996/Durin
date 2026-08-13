#include "SkeletalMesh/SkeletalAssetPostLoad.h"

namespace Durin
{
	namespace
	{
		std::mutex GPayloadLoaderMutex;
		FSkeletalMeshUncookedPayloadLoader GMeshLoader;
		FAnimationClipUncookedPayloadLoader GClipLoader;
	}

	auto RegisterSkeletalAssetUncookedPayloadLoaders(
		FSkeletalMeshUncookedPayloadLoader MeshLoader,
		FAnimationClipUncookedPayloadLoader ClipLoader) -> bool
	{
		if (!MeshLoader || !ClipLoader) return false;
		std::lock_guard Lock(GPayloadLoaderMutex);
		if (GMeshLoader || GClipLoader) return false;
		GMeshLoader = std::move(MeshLoader);
		GClipLoader = std::move(ClipLoader);
		return true;
	}

	auto UnregisterSkeletalAssetUncookedPayloadLoaders() -> void
	{
		std::lock_guard Lock(GPayloadLoaderMutex);
		GMeshLoader = {};
		GClipLoader = {};
	}

	auto InvokeSkeletalMeshUncookedPayloadLoader(
		std::string_view Key,
		const FSkeletalPayloadSerializationContext& Context,
		FSkeletalMeshPayloadData& OutPayload,
		std::string& OutMessage) -> bool
	{
		FSkeletalMeshUncookedPayloadLoader Loader;
		{
			std::lock_guard Lock(GPayloadLoaderMutex);
			Loader = GMeshLoader;
		}
		if (!Loader)
		{
			OutMessage = "No uncooked SkeletalMesh payload policy is registered.";
			return false;
		}
		return Loader(Key, Context, OutPayload, OutMessage);
	}

	auto InvokeAnimationClipUncookedPayloadLoader(
		std::string_view Key,
		const FSkeletalPayloadSerializationContext& Context,
		FAnimationClipPayloadData& OutPayload,
		std::string& OutMessage) -> bool
	{
		FAnimationClipUncookedPayloadLoader Loader;
		{
			std::lock_guard Lock(GPayloadLoaderMutex);
			Loader = GClipLoader;
		}
		if (!Loader)
		{
			OutMessage = "No uncooked AnimationClip payload policy is registered.";
			return false;
		}
		return Loader(Key, Context, OutPayload, OutMessage);
	}
}
