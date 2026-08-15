#include "Texture/TextureBuildFunctionRegistry.h"

#include "Texture/TextureBuildFunctions.h"

namespace Durin::Asset::Build
{
	namespace
	{
		std::mutex GTextureBuildFunctionMutex;
		FBuildFunctionRegistration GTexture2DRegistration;
		FBuildFunctionRegistration GTextureCubeRegistration;
	}

	auto EnsureTextureBuildFunctions(
		std::string* OutError, FModuleOwnedCallbackGate Gate) -> bool
	{
		std::lock_guard Lock(GTextureBuildFunctionMutex);
		if (GTexture2DRegistration.IsValid() && GTextureCubeRegistration.IsValid())
			return true;

		const bool bAcquiredTexture2D = !GTexture2DRegistration.IsValid();
		if (bAcquiredTexture2D)
		{
			GTexture2DRegistration = RegisterBuildFunction(
				Private::Texture2DFunctionIdentity,
				Private::CreateTexture2DBuildFunction(), Gate, OutError);
			if (!GTexture2DRegistration.IsValid()) return false;
		}

		if (!GTextureCubeRegistration.IsValid())
		{
			GTextureCubeRegistration = RegisterBuildFunction(
				Private::TextureCubeFunctionIdentity,
				Private::CreateTextureCubeBuildFunction(), std::move(Gate), OutError);
			if (!GTextureCubeRegistration.IsValid())
			{
				if (bAcquiredTexture2D) GTexture2DRegistration.Reset();
				return false;
			}
		}
		if (OutError) OutError->clear();
		return true;
	}

	auto InitializeTextureBuildFunctions(
		FModuleOwnedCallbackGate Gate, std::string* OutError) -> bool
	{
		return EnsureTextureBuildFunctions(OutError, std::move(Gate));
	}

	auto ShutdownTextureBuildFunctions() -> void
	{
		std::lock_guard Lock(GTextureBuildFunctionMutex);
		GTextureCubeRegistration.Reset();
		GTexture2DRegistration.Reset();
	}
}
