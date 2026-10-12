#pragma once

#include "CoreMinimal.h"

#include "Texture/Texture.h"

namespace Durin
{
	namespace DerivedData { class IRequestOwner; }
#if DURIN_WITH_EDITORONLY_DATA
	// Engine-owned values only. Dispatch starts on a worker; Apply executes on GameThread.
	struct FTexturePlatformCacheResult
	{
		virtual ~FTexturePlatformCacheResult() = default;
		std::string Error;
		bool bCanceled = false;
		virtual auto Apply(DTexture& Texture) -> void = 0;
	};
	struct FTexturePlatformCacheInput
	{
		virtual ~FTexturePlatformCacheInput() = default;
		FTextureSource Source;
		uint64 EstimatedBytes = 0;
		// Complete once on any execution thread. The owner outlives callback retirement.
		virtual auto Dispatch(DerivedData::IRequestOwner& Owner,
			std::function<bool()> ShouldCancel,
			std::function<void(std::unique_ptr<FTexturePlatformCacheResult>)> Completion) const -> void = 0;
	};

	auto SubmitTexturePlatformCache(DTexture& Texture,
		std::shared_ptr<const FTexturePlatformCacheInput> Input) -> bool;
#endif

	auto HasPendingTextureCompilation(const DTexture& Texture) -> bool;
	auto FinishTextureCompilation(DTexture& Texture) -> bool;
}
