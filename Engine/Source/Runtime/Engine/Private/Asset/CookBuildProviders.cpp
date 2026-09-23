#include "CookBuildProviders.h"
#include "StaticMesh/IMeshBuilderModule.h"
#include "Texture/ITextureBuildModule.h"
#include "Physics/PhysicsCookHelper.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::AssetPrivate
{
	auto GetCookBuildProviderInput(std::string_view Family, FByteBuffer& Out) -> bool
	{
		Out.clear();
		if (Family == "texture2d" || Family == "texture-cube" || Family == "volume-texture")
		{
			const auto Module = ITextureBuildModule::Get();
			if (!Module) return false;
			const uint32 BuilderVersion = Family == "texture2d" ? Module->GetTexture2DBuilderVersion()
				: Family == "texture-cube" ? Module->GetTextureCubeBuilderVersion()
				: Module->GetVolumeTextureBuilderVersion();
			if (BuilderVersion == 0) return false;
			FBinaryWriter Writer;
			Writer.WriteU32(BuilderVersion);
			if (Family == "texture-cube")
			{
				const uint32 ProjectionVersion = Module->GetTextureCubeProjectionVersion();
				if (ProjectionVersion == 0) return false;
				Writer.WriteU32(ProjectionVersion);
			}
			Out = Writer.TakeBytes();
			return true;
		}
		if (Family == "static-mesh")
		{
			const auto Module = IMeshBuilderModule::Get();
			if (!Module) return false;
			const uint32 BuilderVersion = Module->GetRenderBuilderVersion();
			if (BuilderVersion == 0) return false;
			FBinaryWriter Writer;
			Writer.WriteU32(BuilderVersion);
			Writer.WriteU32(PhysicsCookBuilderVersion);
			Out = Writer.TakeBytes();
			return true;
		}
		return false;
	}
}
