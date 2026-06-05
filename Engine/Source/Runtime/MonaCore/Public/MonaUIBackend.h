#pragma once

#include "MonaCoreAPI.h"
#include "Math/MathFwd.h"
#include "RHIFwd.h"

namespace Durin::Mona
{
	class IMonaUIBackend
	{
	public:
		virtual ~IMonaUIBackend() = default;

		virtual auto Initialize() -> void = 0;
		virtual auto Shutdown() -> void = 0;
		virtual auto NewFrame() -> void = 0;
		virtual auto Render() -> void = 0;

		virtual auto DrawTexture(FRHITexture* Texture, const FVector2f& Size) -> bool = 0;
	};
}

namespace Durin::MonaUI
{
	MONACORE_API auto DrawTexture(FRHITexture* Texture, const FVector2f& Size) -> bool;
}
