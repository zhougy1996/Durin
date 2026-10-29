#pragma once

#include "CoreMinimal.h"
#include "Asset/DerivedDataCacheKeyProxy.h"
#include "Texture/Texture2DData.h"

#if DURIN_WITH_EDITOR
namespace Durin
{
	struct FTexture2DCacheRequest
	{
		FCacheKeyProxy Key;
		std::span<const uint32> MipIndices;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
		uint64 MaximumEncodedBytes = 2ull * 1024 * 1024 * 1024 + 2 * 1024 * 1024;
		uint64 MaximumWorkingSetBytes = 4ull * 1024 * 1024 * 1024;
		std::function<bool()> ShouldCancel;
	};
	struct FTexture2DCachedMip { uint32 MipIndex = 0; FTexture2DMipData Data; };
	// Selected mips retain their original indices; this is not a complete render product.
	struct FTexture2DCachedMips
	{
		EPixelFormat PixelFormat = EPixelFormat::Unknown;
		uint32 OriginalMipCount = 0;
		std::vector<FTexture2DCachedMip> Mips;
	};
	enum class ETexture2DCacheError : uint8 { InvalidRequest, MissingValue, InvalidOutput, CacheFailure, Canceled };
	struct FTexture2DCacheError { ETexture2DCacheError Code; std::string Diagnostic; };
	// Cache-only: no source resolution or build fallback. An absent manifest is a miss.
	ENGINE_API auto ReadTexture2DCachedMips(const FTexture2DCacheRequest& Request)
		-> std::expected<std::optional<FTexture2DCachedMips>, FTexture2DCacheError>;
}
#endif
