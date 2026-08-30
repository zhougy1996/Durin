#include "Assets/SourceImageThumbnailDiskCache.h"

#include "Hash/XxHash.h"
#include "Image/ImageDecoder.h"
#include "Image/ImageEncoder.h"
#include "Serialization/BinaryFormat.h"
#include "Misc/FileTime.h"
#include "Misc/Paths.h"
#include "Thumbnail/ThumbnailStorage.h"

namespace Durin::Editor::ContentBrowser::Private
{
	namespace
	{
		constexpr uint64 MaximumEncodedObjectBytes = 16ull * 1024ull * 1024ull;

		// Captures source-specific inputs used to derive a provider-neutral object key.
		struct FSourceCacheKeyData
		{
			std::string Key;
			std::string SourceIdentity;
			uint64 SourceSize = 0;
			int64 SourceTimeTicks = 0;
			uint32 MaximumDimension = 0;
			uint32 GeneratorVersion = 0;
			uint32 ColorSpacePolicy = 0;
			uint32 OutputEncodingVersion = 0;
		};

		auto IsContainedBy(const std::filesystem::path& Root, const std::filesystem::path& Candidate) -> bool
		{
			const std::filesystem::path Relative = Candidate.lexically_normal().lexically_relative(Root.lexically_normal());
			if (Relative.empty() || Relative.is_absolute()) return false;
			for (const auto& Part : Relative)
				if (Part == "..") return false;
			return true;
		}

		auto NormalizeSourceIdentity(const std::filesystem::path& PhysicalPath, const std::filesystem::path& OverrideRoot) -> std::string
		{
			std::error_code Error;
			const std::filesystem::path Absolute = std::filesystem::weakly_canonical(PhysicalPath, Error);
			if (Error) return {};
			if (!OverrideRoot.empty())
			{
				const std::filesystem::path Root = std::filesystem::weakly_canonical(OverrideRoot, Error);
				if (!Error && IsContainedBy(Root, Absolute))
					return std::string("$Test/") + Absolute.lexically_relative(Root).generic_string();
				return {};
			}
			return Absolute.generic_string();
		}

		auto DecodeCachedPng(std::span<const std::byte> Bytes, uint32 MaximumDimension,
			FDecodedSourceImageThumbnail& OutThumbnail, std::string& OutError) -> bool
		{
			Image::FDecodedImage Image;
			if (!Image::DecodeImageFromMemory(Bytes, Image, OutError,
				{MaximumEncodedObjectBytes, static_cast<uint64>(MaximumDimension) * MaximumDimension * 4}))
				return false;
			if (Image.Width == 0 || Image.Height == 0 || Image.Width > MaximumDimension || Image.Height > MaximumDimension)
			{
				OutError = "Cached thumbnail dimensions are invalid.";
				return false;
			}
			OutThumbnail.Width = Image.Width;
			OutThumbnail.Height = Image.Height;
			OutThumbnail.Pixels = std::move(Image.Pixels);
			OutThumbnail.bHasTransparency = false;
			for (size_t Index = 3; Index < OutThumbnail.Pixels.size(); Index += 4)
				if (OutThumbnail.Pixels[Index] != std::byte{255})
				{
					OutThumbnail.bHasTransparency = true;
					break;
				}
			return true;
		}

		auto MakeKey(const FSourceCacheKeyData& Entry) -> std::string
		{
			FBinaryWriter Writer;
			Writer.WriteString(Entry.SourceIdentity);
			Writer.WriteU64(Entry.SourceSize);
			Writer.WriteI64(Entry.SourceTimeTicks);
			Writer.WriteU32(Entry.MaximumDimension);
			Writer.WriteU32(Entry.GeneratorVersion);
			Writer.WriteU32(Entry.ColorSpacePolicy);
			Writer.WriteU32(Entry.OutputEncodingVersion);
			return FXxHash128::HashBuffer(Writer.GetBytes()).ToString();
		}
	} // namespace

	// Owns source-specific key generation and delegates persistent storage and budget accounting.
	struct FSourceImageThumbnailDiskCache::FImpl
	{
		explicit FImpl(FSourceImageThumbnailDiskCacheSettings InSettings)
			: Settings(std::move(InSettings))
			, ObjectStore({
				.CacheRoot = Settings.CacheRoot,
				.FormatVersion = Settings.OutputEncodingVersion,
				.DiskBudgetBytes = Settings.DiskBudgetBytes,
				.MaximumObjectBytes = MaximumEncodedObjectBytes,
				.ObjectExtension = ".png"})
		{
		}

		FSourceImageThumbnailDiskCacheSettings Settings;
		::Durin::Editor::FThumbnailObjectStore ObjectStore;
		uint64 SourceDecodes = 0;
		mutable std::mutex Mutex;
	};

	FSourceImageThumbnailDiskCache::FSourceImageThumbnailDiskCache(FSourceImageThumbnailDiskCacheSettings Settings)
		: Impl(std::make_unique<FImpl>(std::move(Settings)))
	{
	}

	FSourceImageThumbnailDiskCache::~FSourceImageThumbnailDiskCache() = default;

	auto FSourceImageThumbnailDiskCache::LoadOrGenerate(std::string_view PhysicalPath, uintmax_t FileSize,
		const std::filesystem::file_time_type& LastWriteTime, FDecodedSourceImageThumbnail& OutThumbnail,
		std::string& OutError) -> bool
	{
		OutThumbnail = {};
		OutError.clear();
		std::error_code SourceError;
		const std::filesystem::path SourcePath(PhysicalPath);
		const uintmax_t CurrentFileSize = std::filesystem::file_size(SourcePath, SourceError);
		if (SourceError)
		{
			OutError = "The source image no longer exists.";
			return false;
		}
		const std::filesystem::file_time_type CurrentLastWriteTime = std::filesystem::last_write_time(SourcePath, SourceError);
		if (SourceError)
		{
			OutError = "Unable to read the source image timestamp.";
			return false;
		}
		const bool bFingerprintChanged = CurrentFileSize != FileSize || CurrentLastWriteTime != LastWriteTime;
		const uintmax_t EffectiveFileSize = bFingerprintChanged ? CurrentFileSize : FileSize;
		const std::filesystem::file_time_type& EffectiveLastWriteTime = bFingerprintChanged ? CurrentLastWriteTime : LastWriteTime;
		FSourceCacheKeyData Desired{
			.SourceIdentity = NormalizeSourceIdentity(std::filesystem::path(PhysicalPath), Impl->Settings.SourceIdentityRoot),
			.SourceSize = static_cast<uint64>(EffectiveFileSize),
			.SourceTimeTicks = FileTime::ToStableTicks(EffectiveLastWriteTime),
			.MaximumDimension = Impl->Settings.MaximumDimension,
			.GeneratorVersion = Impl->Settings.GeneratorVersion,
			.ColorSpacePolicy = Impl->Settings.ColorSpacePolicy,
			.OutputEncodingVersion = Impl->Settings.OutputEncodingVersion,
		};
		Desired.Key = MakeKey(Desired);
		if (!Desired.SourceIdentity.empty())
		{
			std::vector<std::byte> EncodedBytes;
			if (Impl->ObjectStore.Load(Desired.Key, EncodedBytes) == ::Durin::Editor::EThumbnailObjectLoadResult::Hit)
			{
				if (DecodeCachedPng(EncodedBytes, Impl->Settings.MaximumDimension, OutThumbnail, OutError))
					return true;
				Impl->ObjectStore.Invalidate(Desired.Key);
			}
		}

		{
			std::lock_guard Lock(Impl->Mutex);
			++Impl->SourceDecodes;
		}
		if (!DecodeSourceImageThumbnail(PhysicalPath, Impl->Settings.MaximumDimension, OutThumbnail, OutError)) return false;
		if (Desired.SourceIdentity.empty()) return true;

		std::vector<std::byte> EncodedBytes;
		if (!Image::EncodeRgba8Png(
				OutThumbnail.Pixels, OutThumbnail.Width, OutThumbnail.Height, EncodedBytes)) return true;
		Impl->ObjectStore.Store(Desired.Key, EncodedBytes);
		return true;
	}

	auto FSourceImageThumbnailDiskCache::GetStats() const -> FSourceImageThumbnailDiskCacheStats
	{
		const ::Durin::Editor::FThumbnailObjectStoreStats StoreStats = Impl->ObjectStore.GetStats();
		std::lock_guard Lock(Impl->Mutex);
		return {
			.CacheHits = StoreStats.CacheHits,
			.SourceDecodes = Impl->SourceDecodes,
			.Regenerations = StoreStats.Regenerations};
	}
} // namespace Durin::Editor::ContentBrowser::Private
