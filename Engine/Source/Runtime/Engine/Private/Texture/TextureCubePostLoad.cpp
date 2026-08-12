#include "Texture/TextureCubePostLoad.h"

namespace Durin
{
	namespace
	{
		std::mutex GMutex;
		FTextureCubeUncookedPostLoadHandler GHandler;
	}

	auto RegisterTextureCubeUncookedPostLoadHandler(
		FTextureCubeUncookedPostLoadHandler Handler) -> bool
	{
		if (!Handler) return false;
		std::lock_guard Lock(GMutex);
		if (GHandler) return false;
		GHandler = std::move(Handler);
		return true;
	}

	auto UnregisterTextureCubeUncookedPostLoadHandler() -> void
	{
		std::lock_guard Lock(GMutex);
		GHandler = {};
	}

	auto InvokeTextureCubeUncookedPostLoadHandler(
		DTextureCube& Texture, std::string& OutError) -> bool
	{
		FTextureCubeUncookedPostLoadHandler Handler;
		{
			std::lock_guard Lock(GMutex);
			Handler = GHandler;
		}
		if (!Handler)
		{
			OutError = "No uncooked TextureCube load policy is registered.";
			return false;
		}
		return Handler(Texture, OutError);
	}
}
