#include "CookBuildProviders.h"
#include "Texture/Texture2DBuild.h"
#include "Texture/TextureCubeBuild.h"
#include "Texture/VolumeTextureBuild.h"
#include "StaticMesh/IMeshBuilderModule.h"
#include "Texture/ITextureBuildModule.h"
#include "Physics/PhysicsCookHelper.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::AssetPrivate
{
	namespace
	{
		template<typename TDescriptor>
		auto EncodeDescriptor(const TDescriptor& Descriptor) -> FByteBuffer
		{
			if (!Descriptor.IsValid() || Descriptor.ProducerIdentity.size() > 4096) return {};
			FBinaryWriter Writer;
			Writer.WriteString(Descriptor.ProducerIdentity);
			if constexpr (requires { Descriptor.BuilderVersion; }) Writer.WriteU32(Descriptor.BuilderVersion);
			if constexpr (requires { Descriptor.ProjectionVersion; }) Writer.WriteU32(Descriptor.ProjectionVersion);
			return Writer.TakeBytes();
		}

	}

	auto GetCookBuildProviderInput(std::string_view Family, FByteBuffer& Out) -> bool
	{
		Out.clear();
		if (Family == "texture2d" || Family == "texture-cube" || Family == "volume-texture")
		{
			const auto Module = ITextureBuildModule::Get();
			if (!Module) return false;
			if (Family == "texture2d") Out = EncodeDescriptor(Module->GetTexture2DDescriptor());
			else if (Family == "texture-cube") Out = EncodeDescriptor(Module->GetTextureCubeDescriptor());
			else Out = EncodeDescriptor(Module->GetVolumeTextureDescriptor());
			return !Out.empty();
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
