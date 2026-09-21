#pragma once

#include "Texture/Texture.h"

namespace Durin
{
	// Engine-owned values only. Build executes on a worker; Apply executes on GameThread.
	struct FTexturePlatformCacheResult
	{
		virtual ~FTexturePlatformCacheResult() = default;
		std::string Error;
		virtual auto Apply(DTexture& Texture) -> void = 0;
	};
	struct FTexturePlatformCacheInput
	{
		virtual ~FTexturePlatformCacheInput() = default;
		FTextureSource Source;
		uint64 EstimatedBytes = 0;
		virtual auto Build() const -> std::unique_ptr<FTexturePlatformCacheResult> = 0;
	};

	auto SubmitTexturePlatformCache(DTexture& Texture,
		std::shared_ptr<const FTexturePlatformCacheInput> Input) -> bool;
	auto HasPendingTextureCompilation(const DTexture& Texture) -> bool;
	auto FinishTextureCompilation(DTexture& Texture) -> bool;
}
