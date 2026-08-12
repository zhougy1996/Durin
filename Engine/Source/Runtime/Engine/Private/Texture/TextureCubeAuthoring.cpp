#include "Texture/TextureCubeAuthoring.h"

namespace Durin
{
	namespace
	{
		std::mutex GMutex;
		FTextureCubeAuthoringHandlers GHandlers;
	}

	auto RegisterTextureCubeAuthoringHandlers(FTextureCubeAuthoringHandlers Handlers) -> bool
	{
		if (!Handlers.BuildPanorama || !Handlers.BuildFaces || !Handlers.Rebuild
			|| !Handlers.ValidateFaces || !Handlers.ValidatePanorama) return false;
		std::lock_guard Lock(GMutex);
		if (GHandlers.BuildPanorama) return false;
		GHandlers = std::move(Handlers);
		return true;
	}

	auto UnregisterTextureCubeAuthoringHandlers() -> void
	{
		std::lock_guard Lock(GMutex);
		GHandlers = {};
	}

	auto GetTextureCubeAuthoringHandlers() -> FTextureCubeAuthoringHandlers
	{
		std::lock_guard Lock(GMutex);
		return GHandlers;
	}
}
