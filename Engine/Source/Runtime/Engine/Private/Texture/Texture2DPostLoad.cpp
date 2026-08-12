#include "Texture/Texture2DPostLoad.h"

#include "Texture/Texture2D.h"

namespace Durin
{
	namespace
	{
		std::mutex GTexture2DPostLoadHandlerMutex;
		FTexture2DUncookedPostLoadHandler GTexture2DPostLoadHandler;
	}

	auto RegisterTexture2DUncookedPostLoadHandler(
		FTexture2DUncookedPostLoadHandler Handler) -> bool
	{
		if (!Handler) return false;
		std::lock_guard Lock(GTexture2DPostLoadHandlerMutex);
		if (GTexture2DPostLoadHandler) return false;
		GTexture2DPostLoadHandler = std::move(Handler);
		return true;
	}

	auto UnregisterTexture2DUncookedPostLoadHandler() -> void
	{
		std::lock_guard Lock(GTexture2DPostLoadHandlerMutex);
		GTexture2DPostLoadHandler = {};
	}

	auto InvokeTexture2DUncookedPostLoadHandler(
		DTexture2D& Texture, std::string& OutError) -> bool
	{
		FTexture2DUncookedPostLoadHandler Handler;
		{
			std::lock_guard Lock(GTexture2DPostLoadHandlerMutex);
			Handler = GTexture2DPostLoadHandler;
		}
		if (!Handler)
		{
			OutError = "No uncooked Texture2D load policy is registered.";
			return false;
		}
		return Handler(Texture, OutError);
	}
}
