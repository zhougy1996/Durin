#include "TargetPlatform.h"
#include "Modules/ModuleManager.h"

namespace Durin
{
	auto ResolveTargetPlatform(ECookTargetPlatform Platform)
		-> std::expected<FTargetPlatformDescription, ETargetPlatformError>
	{
		if (GetTargetPlatformName(Platform).empty()) return std::unexpected(ETargetPlatformError::UnknownPlatform);
		const auto Shader = Platform == ECookTargetPlatform::MacOS ? MetalShaderTarget : VulkanShaderTarget;
		return FTargetPlatformDescription(Platform, Shader);
	}
	auto FTargetPlatformDescription::SupportsCollisionPayload() const -> bool
	{
		return Platform == ECookTargetPlatform::Win64;
	}
	class FTargetPlatformModule final : public IModuleInterface {};
	IMPLEMENT_MODULE(FTargetPlatformModule, TargetPlatform)
}
