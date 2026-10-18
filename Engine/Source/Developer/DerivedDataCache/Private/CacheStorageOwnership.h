#pragma once

#include "CoreMinimal.h"
#include "DerivedDataCache/DerivedDataCacheTypes.h"

namespace Durin::DerivedData::Private
{
	class FCacheStorageLease
	{
	public:
		DERIVEDDATACACHE_API ~FCacheStorageLease();
		std::filesystem::path Root;
	private:
		friend auto OpenCacheStorageLease(const std::filesystem::path&, bool) -> std::expected<std::shared_ptr<FCacheStorageLease>, FCacheError>;
		intptr_t Handle = -1;
	};
	inline constexpr std::string_view CacheLockName = ".durin-ddc-v1.lock";
	DERIVEDDATACACHE_API auto OpenCacheStorageLease(const std::filesystem::path& Root, bool Exclusive)
		-> std::expected<std::shared_ptr<FCacheStorageLease>, FCacheError>;
	// Process ownership follows the configured root. Callers retain their lease
	// through the complete record transaction, including blob-before-manifest Put.
	auto AcquireCacheStorageOwnership() -> std::expected<std::shared_ptr<FCacheStorageLease>, FCacheError>;
	// Shutdown/test use only, after cache users have drained. Never removes the lock file.
	DERIVEDDATACACHE_API auto ReleaseCacheStorageOwnership() -> void;
}
