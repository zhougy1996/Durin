#pragma once

#include "MonaCoreAPI.h"
#include "Math/MathFwd.h"
#include "RHIResources.h"

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
		virtual auto RegisterTexture(const FTextureRHIRef& Texture) -> void = 0;
		virtual auto UnregisterTexture(const FTextureRHIRef& Texture) -> void = 0;
		virtual auto DrawTexture(const FTextureRHIRef& Texture, const FVector2f& Size) -> bool = 0;
	};
}
