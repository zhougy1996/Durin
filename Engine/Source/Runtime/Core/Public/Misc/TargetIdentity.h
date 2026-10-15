#pragma once

#include "Misc/CoreStd.h"
#include "Misc/CoreTypes.h"
#include "CoreAPI.h"

namespace Durin
{
	// Stable disk identities. Distinct enum types preserve existing public ABI.
	enum class ECookTargetPlatform : uint32 { Invalid = 0, Win64 = 1, MacOS = 2 };
	enum class ECookTargetProfile : uint32 { Invalid = 0, Game = 1, EditorValidation = 2 };
	enum class EAssetPayloadTargetPlatform : uint32 { Unknown = 0, Win64 = 1, MacOS = 2 };
	enum class EShaderTargetPlatform : uint32 { Invalid = 0, Win64 = 1, MacOS = 2 };
	enum class EShaderTargetProfile : uint32 { Invalid = 0, Game = 1, EditorValidation = 2 };

	// Parsing and adaptation reject unknown values; names of unknown values are empty.
	enum class ETargetIdentityError : uint8 { UnknownPlatform, UnknownProfile };
	CORE_API auto ParseTargetPlatform(std::string_view Name) -> std::expected<ECookTargetPlatform, ETargetIdentityError>;
	CORE_API auto ParseTargetProfile(std::string_view Name) -> std::expected<ECookTargetProfile, ETargetIdentityError>;
	CORE_API auto GetTargetPlatformName(ECookTargetPlatform Platform) -> std::string_view;
	CORE_API auto GetTargetProfileName(ECookTargetProfile Profile) -> std::string_view;
	CORE_API auto ToCookTargetPlatform(EShaderTargetPlatform Platform) -> std::expected<ECookTargetPlatform, ETargetIdentityError>;
	CORE_API auto ToShaderTargetPlatform(ECookTargetPlatform Platform) -> std::expected<EShaderTargetPlatform, ETargetIdentityError>;
	CORE_API auto ToShaderTargetProfile(ECookTargetProfile Profile) -> std::expected<EShaderTargetProfile, ETargetIdentityError>;
}
