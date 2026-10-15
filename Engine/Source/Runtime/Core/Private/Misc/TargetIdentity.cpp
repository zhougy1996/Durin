#include "Misc/TargetIdentity.h"

namespace Durin
{
	auto ParseTargetPlatform(std::string_view Name) -> std::expected<ECookTargetPlatform, ETargetIdentityError>
	{
		if (Name == "Win64" || Name == "win64") return ECookTargetPlatform::Win64;
		if (Name == "MacOS" || Name == "macos") return ECookTargetPlatform::MacOS;
		return std::unexpected(ETargetIdentityError::UnknownPlatform);
	}
	auto ParseTargetProfile(std::string_view Name) -> std::expected<ECookTargetProfile, ETargetIdentityError>
	{
		if (Name == "Game" || Name == "game") return ECookTargetProfile::Game;
		if (Name == "EditorValidation" || Name == "editorvalidation") return ECookTargetProfile::EditorValidation;
		return std::unexpected(ETargetIdentityError::UnknownProfile);
	}
	auto GetTargetPlatformName(ECookTargetPlatform Platform) -> std::string_view
	{
		switch (Platform) {
		case ECookTargetPlatform::Win64: return "Win64";
		case ECookTargetPlatform::MacOS: return "MacOS";
		default: return {};
		}
	}
	auto GetTargetProfileName(ECookTargetProfile Profile) -> std::string_view
	{
		switch (Profile) {
		case ECookTargetProfile::Game: return "Game";
		case ECookTargetProfile::EditorValidation: return "EditorValidation";
		default: return {};
		}
	}
	auto ToCookTargetPlatform(EShaderTargetPlatform Platform) -> std::expected<ECookTargetPlatform, ETargetIdentityError>
	{
		switch (Platform) {
		case EShaderTargetPlatform::Win64: return ECookTargetPlatform::Win64;
		case EShaderTargetPlatform::MacOS: return ECookTargetPlatform::MacOS;
		default: return std::unexpected(ETargetIdentityError::UnknownPlatform);
		}
	}
	auto ToShaderTargetPlatform(ECookTargetPlatform Platform) -> std::expected<EShaderTargetPlatform, ETargetIdentityError>
	{
		switch (Platform) {
		case ECookTargetPlatform::Win64: return EShaderTargetPlatform::Win64;
		case ECookTargetPlatform::MacOS: return EShaderTargetPlatform::MacOS;
		default: return std::unexpected(ETargetIdentityError::UnknownPlatform);
		}
	}
	auto ToShaderTargetProfile(ECookTargetProfile Profile) -> std::expected<EShaderTargetProfile, ETargetIdentityError>
	{
		switch (Profile) {
		case ECookTargetProfile::Game: return EShaderTargetProfile::Game;
		case ECookTargetProfile::EditorValidation: return EShaderTargetProfile::EditorValidation;
		default: return std::unexpected(ETargetIdentityError::UnknownProfile);
		}
	}
}
