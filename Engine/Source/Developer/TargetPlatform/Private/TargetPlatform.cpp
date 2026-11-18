#include "TargetPlatform.h"
#include "Modules/ModuleManager.h"

namespace Durin
{
	namespace
	{
		template<ECookTargetPlatform Platform>
		class TDesktopTargetPlatform final : public ITargetPlatform
		{
		public:
			auto GetPlatform() const -> ECookTargetPlatform override { return Platform; }
			auto GetShaderTarget() const -> const FShaderTargetIdentity& override
			{ return Platform == ECookTargetPlatform::MacOS ? MetalShaderTarget : VulkanShaderTarget; }
			auto GetCookedShaderTarget() const -> FShaderTargetIdentity override
			{ return Platform == ECookTargetPlatform::MacOS ? MetalLibraryShaderTarget : VulkanShaderTarget; }
			auto GetTextureFormat(ETargetTextureUsage Usage, bool bSRGB, bool bHasTransparency) const -> EPixelFormat override
			{
				switch (Usage)
				{
				case ETargetTextureUsage::Color:
					if (bHasTransparency) return bSRGB ? EPixelFormat::BC3_UNORM_SRGB : EPixelFormat::BC3_UNORM;
					return bSRGB ? EPixelFormat::BC1_UNORM_SRGB : EPixelFormat::BC1_UNORM;
				case ETargetTextureUsage::Normal: return EPixelFormat::BC5_UNORM;
				case ETargetTextureUsage::DataMask: return bSRGB ? EPixelFormat::BC7_UNORM_SRGB : EPixelFormat::BC7_UNORM;
				default: return EPixelFormat::Unknown;
				}
			}
			auto SupportsCollisionPayload() const -> bool override { return Platform == ECookTargetPlatform::Win64; }
		};
	}

	auto ResolveTargetPlatform(ECookTargetPlatform Platform) -> const ITargetPlatform*
	{
		static const TDesktopTargetPlatform<ECookTargetPlatform::Win64> Windows;
		static const TDesktopTargetPlatform<ECookTargetPlatform::MacOS> Mac;
		switch (Platform)
		{
		case ECookTargetPlatform::Win64: return &Windows;
		case ECookTargetPlatform::MacOS: return &Mac;
		default: return nullptr;
		}
	}
	class FTargetPlatformModule final : public IModuleInterface {};
	IMPLEMENT_MODULE(FTargetPlatformModule, TargetPlatform)
}
