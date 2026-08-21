#include "Texture/TextureBuildFunctionRegistry.h"

#include "Texture/TextureBuildFunctions.h"

namespace Durin::Asset::Build
{
	namespace
	{
		std::mutex GTextureBuildFunctionMutex;
		FBuildFunctionRegistration GTexture2DRegistration;
		FBuildFunctionRegistration GTextureCubeRegistration;
		FBuildFunctionRegistration GVolumeTextureRegistration;
	}

	auto EnsureTextureBuildFunctions(
		std::string* OutError, FModuleOwnedCallbackGate Gate) -> bool
	{
		std::lock_guard Lock(GTextureBuildFunctionMutex);
		if (GTexture2DRegistration.IsValid() && GTextureCubeRegistration.IsValid()
			&& GVolumeTextureRegistration.IsValid())
			return true;

		const bool bAcquiredTexture2D = !GTexture2DRegistration.IsValid();
		if (bAcquiredTexture2D)
		{
			GTexture2DRegistration = RegisterBuildFunction(
				Private::Texture2DFunctionIdentity,
				Private::CreateTexture2DBuildFunction(), Gate, OutError);
			if (!GTexture2DRegistration.IsValid()) return false;
		}

		const bool bAcquiredTextureCube = !GTextureCubeRegistration.IsValid();
		if (bAcquiredTextureCube)
		{
			GTextureCubeRegistration = RegisterBuildFunction(
				Private::TextureCubeFunctionIdentity,
				Private::CreateTextureCubeBuildFunction(), Gate, OutError);
			if (!GTextureCubeRegistration.IsValid())
			{
				if (bAcquiredTexture2D) GTexture2DRegistration.Reset();
				return false;
			}
		}
		if (!GVolumeTextureRegistration.IsValid())
		{
			GVolumeTextureRegistration = RegisterBuildFunction(
				Private::VolumeTextureFunctionIdentity,
				Private::CreateVolumeTextureBuildFunction(), std::move(Gate), OutError);
			if (!GVolumeTextureRegistration.IsValid())
			{
				if (bAcquiredTextureCube) GTextureCubeRegistration.Reset();
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
		GVolumeTextureRegistration.Reset();
		GTextureCubeRegistration.Reset();
		GTexture2DRegistration.Reset();
	}
}
