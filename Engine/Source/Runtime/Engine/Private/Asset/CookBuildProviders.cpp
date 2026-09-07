#include "CookBuildProviders.h"
#include "Texture/Texture2DBuildProvider.h"
#include "Texture/TextureCubeBuildProvider.h"
#include "Texture/VolumeTextureBuildProvider.h"
#include "StaticMesh/StaticMeshBuildProvider.h"
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
			if constexpr (requires { Descriptor.RenderBuilderVersion; }) Writer.WriteU32(Descriptor.RenderBuilderVersion);
			if constexpr (requires { Descriptor.CollisionBuilderVersion; }) Writer.WriteU32(Descriptor.CollisionBuilderVersion);
			return Writer.TakeBytes();
		}

		template<typename TProvider>
		auto ReadDescriptor(FByteBuffer& Out) -> bool
		{
			const auto Result = FModularFeatureRegistry::Get().InvokeSingle<TProvider>(
				[](TProvider& Provider) { return EncodeDescriptor(Provider.GetDescriptor()); });
			if (Result.Status != EFeatureInvokeStatus::Invoked || !Result.Value) return false;
			Out = *Result.Value;
			return !Out.empty();
		}
	}

	auto GetCookBuildProviderInput(std::string_view Family, FByteBuffer& Out) -> bool
	{
		Out.clear();
		if (Family == "texture2d") return ReadDescriptor<ITexture2DBuildProvider>(Out);
		if (Family == "texture-cube") return ReadDescriptor<ITextureCubeBuildProvider>(Out);
		if (Family == "volume-texture") return ReadDescriptor<IVolumeTextureBuildProvider>(Out);
		if (Family == "static-mesh") return ReadDescriptor<IStaticMeshBuildProvider>(Out);
		return false;
	}
}
