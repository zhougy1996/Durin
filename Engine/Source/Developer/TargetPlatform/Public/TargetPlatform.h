#pragma once

#include "CoreMinimal.h"
#include "Misc/TargetIdentity.h"
#include "RHIDefinitions.h"
#include "PixelFormat.h"
#include "TargetPlatformAPI.h"

namespace Durin
{
	enum class ETargetTextureUsage : uint8 { Color, Normal, DataMask };

	class ITargetPlatform
	{
	public:
		virtual ~ITargetPlatform() = default;
		virtual auto GetPlatform() const -> ECookTargetPlatform = 0;
		virtual auto GetShaderTarget() const -> const FShaderTargetIdentity& = 0;
		virtual auto GetCookedShaderTarget() const -> FShaderTargetIdentity = 0;
		virtual auto GetTextureFormat(ETargetTextureUsage Usage, bool bSRGB, bool bHasTransparency) const -> EPixelFormat = 0;
		virtual auto SupportsCollisionPayload() const -> bool = 0;
	};

	TARGETPLATFORM_API auto ResolveTargetPlatform(ECookTargetPlatform Platform) -> const ITargetPlatform*;
}
