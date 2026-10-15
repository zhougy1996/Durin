#pragma once

#include "CoreMinimal.h"
#include "Misc/TargetIdentity.h"
#include "RHIDefinitions.h"
#include "TargetPlatformAPI.h"

namespace Durin
{
	enum class ETargetPlatformError : uint8 { UnknownPlatform };

	// Immutable resolved policy. Family algorithms and cache identity stay with consumers.
	class FTargetPlatformDescription
	{
	public:
		auto GetPlatform() const -> ECookTargetPlatform { return Platform; }
		auto GetShaderTarget() const -> const FShaderTargetIdentity& { return ShaderTarget; }
		TARGETPLATFORM_API auto SupportsCollisionPayload() const -> bool;
	private:
		friend TARGETPLATFORM_API auto ResolveTargetPlatform(ECookTargetPlatform)
			-> std::expected<FTargetPlatformDescription, ETargetPlatformError>;
		FTargetPlatformDescription(ECookTargetPlatform Platform, FShaderTargetIdentity Shader)
			: Platform(Platform), ShaderTarget(Shader) {}
		ECookTargetPlatform Platform;
		FShaderTargetIdentity ShaderTarget;
	};

	// Pure resolution does not load providers, query RHI devices, or require a compiler.
	TARGETPLATFORM_API auto ResolveTargetPlatform(ECookTargetPlatform Platform)
		-> std::expected<FTargetPlatformDescription, ETargetPlatformError>;
}
