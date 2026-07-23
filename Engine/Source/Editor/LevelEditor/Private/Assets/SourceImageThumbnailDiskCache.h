#pragma once

#include "Assets/SourceImageThumbnailDecoder.h"

namespace Durin
{
	struct FSourceImageThumbnailDiskCacheSettings
	{
		std::filesystem::path CacheRoot;
		std::filesystem::path SourceIdentityRoot;
		uint32 MaximumDimension = 256;
		uint32 GeneratorVersion = 1;
		uint32 ColorSpacePolicy = 1;
		uint32 OutputEncodingVersion = 1;
		uint64 DiskBudgetBytes = 256ull * 1024ull * 1024ull;
	};

	struct FSourceImageThumbnailDiskCacheStats
	{
		uint64 CacheHits = 0;
		uint64 SourceDecodes = 0;
		uint64 Regenerations = 0;
	};

	class FSourceImageThumbnailDiskCache
	{
	public:
		explicit FSourceImageThumbnailDiskCache(FSourceImageThumbnailDiskCacheSettings Settings = {});
		~FSourceImageThumbnailDiskCache();

		FSourceImageThumbnailDiskCache(const FSourceImageThumbnailDiskCache&) = delete;
		FSourceImageThumbnailDiskCache& operator=(const FSourceImageThumbnailDiskCache&) = delete;

		auto LoadOrGenerate(std::string_view PhysicalPath, uintmax_t FileSize,
			const std::filesystem::file_time_type& LastWriteTime, FDecodedSourceImageThumbnail& OutThumbnail,
			std::string& OutError) -> bool;
		auto GetStats() const -> FSourceImageThumbnailDiskCacheStats;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
} // namespace Durin
