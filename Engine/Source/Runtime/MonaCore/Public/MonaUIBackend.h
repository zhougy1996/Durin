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
		virtual auto IsTextureRegistered(const FRHITexture* InTexture) -> bool = 0;

		// Returns true if the image was successfully drawn, false otherwise (e.g. if the texture was not registered or if the backend does not support direct image drawing).
		virtual auto DrawImage(const FRHITexture*, const FVector2f& Size) -> bool = 0;
	};
}
