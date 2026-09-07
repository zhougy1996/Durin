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
		thread_local std::map<std::string, FByteBuffer> CapturedDescriptors;
		thread_local std::vector<std::function<bool()>> Validators;

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
		auto CaptureProvider(std::string Family, const std::function<bool()>& Work, std::string& Error) -> bool
		{
			const auto Selected = FModularFeatureRegistry::Get().InvokeSingle<TProvider>(
				[](TProvider& Provider) { return EncodeDescriptor(Provider.GetDescriptor()); });
			if (Selected.MatchingRegistrationCount == 0) return Work();
			if (Selected.Status != EFeatureInvokeStatus::Invoked || !Selected.Value || Selected.Value->empty())
			{
				Error = "Cook recipe provider is ambiguous or invalid: " + Family;
				return false;
			}
			const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<TProvider>([&](TProvider&) {
				CapturedDescriptors.emplace(Family, *Selected.Value);
				Validators.push_back([Identity = Selected.RegistrationIdentity, Expected = *Selected.Value] {
					const auto Current = FModularFeatureRegistry::Get().InvokeSingle<TProvider>(
						[](TProvider& Provider) { return EncodeDescriptor(Provider.GetDescriptor()); }, Identity);
					return Current.Status == EFeatureInvokeStatus::Invoked && Current.Value && *Current.Value == Expected;
				});
				struct FRelease
				{
					std::string Family;
					~FRelease() { Validators.pop_back(); CapturedDescriptors.erase(Family); }
				} Release{Family};
				return Work();
			}, Selected.RegistrationIdentity);
			if (Invocation.Status == EFeatureInvokeStatus::Invoked && Invocation.Value) return *Invocation.Value;
			Error = "Cook recipe provider changed or failed: " + Family;
			return false;
		}
	}

	auto WithCapturedCookBuildProviders(const std::function<bool()>& Work, std::string& Error) -> bool
	{
		return CaptureProvider<ITexture2DBuildProvider>("texture2d", [&] {
			return CaptureProvider<ITextureCubeBuildProvider>("texture-cube", [&] {
				return CaptureProvider<IVolumeTextureBuildProvider>("volume-texture", [&] {
					return CaptureProvider<IStaticMeshBuildProvider>("static-mesh", Work, Error);
				}, Error);
			}, Error);
		}, Error);
	}

	auto VerifyCapturedCookBuildProviders() -> bool
	{
		for (const auto& Verify : Validators) if (!Verify()) return false;
		return true;
	}

	auto GetCapturedCookBuildProviderInput(std::string_view Family, FByteBuffer& Out) -> bool
	{
		const auto Found = CapturedDescriptors.find(std::string(Family));
		if (Found == CapturedDescriptors.end()) { Out.clear(); return false; }
		Out = Found->second;
		return true;
	}
}
