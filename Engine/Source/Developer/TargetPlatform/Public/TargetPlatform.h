#pragma once

#include "CoreMinimal.h"
#include "Misc/TargetIdentity.h"
#include "RHIDefinitions.h"
#include "TargetPlatformAPI.h"

namespace Durin
{
	// Operation capabilities describe representations, not compiler/SDK availability.
	enum class ETargetOperation : uint8 { Cook, TextureMetadata, TextureProduction, CollisionPayload };
	enum class ETargetPlatformError : uint8 { UnknownPlatform, UnknownProfile };

	// Immutable resolved policy. Family algorithms and cache identity stay with consumers.
	class FTargetPlatformDescription
	{
	public:
		auto GetPlatform() const -> ECookTargetPlatform { return Platform; }
		auto GetProfile() const -> ECookTargetProfile { return Profile; }
		auto GetShaderTarget() const -> const FShaderTargetIdentity& { return ShaderTarget; }
		TARGETPLATFORM_API auto Supports(ETargetOperation Operation) const -> bool;
	private:
		friend TARGETPLATFORM_API auto ResolveTargetPlatform(ECookTargetPlatform, ECookTargetProfile)
			-> std::expected<FTargetPlatformDescription, ETargetPlatformError>;
		FTargetPlatformDescription(ECookTargetPlatform Platform, ECookTargetProfile Profile, FShaderTargetIdentity Shader)
			: Platform(Platform), Profile(Profile), ShaderTarget(Shader) {}
		ECookTargetPlatform Platform;
		ECookTargetProfile Profile;
		FShaderTargetIdentity ShaderTarget;
	};

	// Pure resolution does not load providers, query RHI devices, or require a compiler.
	TARGETPLATFORM_API auto ResolveTargetPlatform(ECookTargetPlatform Platform, ECookTargetProfile Profile)
		-> std::expected<FTargetPlatformDescription, ETargetPlatformError>;
}
