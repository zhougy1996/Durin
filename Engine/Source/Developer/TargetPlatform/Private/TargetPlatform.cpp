#include "TargetPlatform.h"
#include "Modules/ModuleManager.h"

namespace Durin
{
	auto ResolveTargetPlatform(ECookTargetPlatform Platform, ECookTargetProfile Profile)
		-> std::expected<FTargetPlatformDescription, ETargetPlatformError>
	{
		if (GetTargetPlatformName(Platform).empty()) return std::unexpected(ETargetPlatformError::UnknownPlatform);
		if (GetTargetProfileName(Profile).empty()) return std::unexpected(ETargetPlatformError::UnknownProfile);
		const auto Shader = Platform == ECookTargetPlatform::MacOS ? MetalShaderTarget : VulkanShaderTarget;
		return FTargetPlatformDescription(Platform, Profile, Shader);
	}
	auto FTargetPlatformDescription::Supports(ETargetOperation Operation) const -> bool
	{
		switch (Operation) {
		case ETargetOperation::TextureMetadata: return true;
		case ETargetOperation::Cook:
		case ETargetOperation::TextureProduction: return Profile == ECookTargetProfile::Game;
		case ETargetOperation::CollisionPayload: return Platform == ECookTargetPlatform::Win64;
		default: return false;
		}
	}
	class FTargetPlatformModule final : public IModuleInterface {};
	IMPLEMENT_MODULE(FTargetPlatformModule, TargetPlatform)
}
