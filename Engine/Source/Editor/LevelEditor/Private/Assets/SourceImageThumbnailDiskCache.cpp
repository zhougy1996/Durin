#include "Assets/SourceImageThumbnailDiskCache.h"

#include "Hash/XxHash.h"
#include "Image/ImageDecoder.h"
#include "Misc/DerivedDataCache.h"
#include "Misc/Paths.h"
#include "Thumbnail/AssetThumbnailObjectStore.h"

namespace Durin::Editor::Level
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
			const PathUtilities::FSourcePathResult Classified =
				PathUtilities::ClassifySourcePath(Absolute);
			return Classified ? Classified.NormalizedVirtualPath : std::string{};
		}

		auto AppendBigEndian(std::vector<uint8>& Bytes, uint32 Value) -> void
		{
			Bytes.push_back(static_cast<uint8>(Value >> 24));
			Bytes.push_back(static_cast<uint8>(Value >> 16));
			Bytes.push_back(static_cast<uint8>(Value >> 8));
			Bytes.push_back(static_cast<uint8>(Value));
		}

		auto Crc32(std::span<const uint8> Bytes) -> uint32
		{
			uint32 Crc = 0xffffffffu;
			for (const uint8 Byte : Bytes)
			{
				Crc ^= Byte;
				for (uint32 Bit = 0; Bit < 8; ++Bit)
					Crc = (Crc >> 1) ^ (0xedb88320u & (0u - (Crc & 1u)));
			}
			return ~Crc;
		}

		auto WritePngChunk(std::vector<uint8>& Bytes, std::string_view Type, std::span<const uint8> Payload) -> void
		{
			AppendBigEndian(Bytes, static_cast<uint32>(Payload.size()));
			const size_t CrcStart = Bytes.size();
			Bytes.insert(Bytes.end(), Type.begin(), Type.end());
			Bytes.insert(Bytes.end(), Payload.begin(), Payload.end());
			AppendBigEndian(Bytes, Crc32(std::span(Bytes).subspan(CrcStart)));
		}

		auto EncodeRgbaPng(const FDecodedSourceImageThumbnail& Thumbnail, std::vector<uint8>& OutBytes) -> bool
		{
			const uint64 ExpectedBytes = static_cast<uint64>(Thumbnail.Width) * Thumbnail.Height * 4;
			if (Thumbnail.Width == 0 || Thumbnail.Height == 0 || Thumbnail.Pixels.size() != ExpectedBytes) return false;

			std::vector<uint8> Scanlines;
			Scanlines.reserve(static_cast<size_t>(ExpectedBytes) + Thumbnail.Height);
			const size_t RowBytes = static_cast<size_t>(Thumbnail.Width) * 4;
			for (uint32 Y = 0; Y < Thumbnail.Height; ++Y)
			{
				Scanlines.push_back(0);
				Scanlines.insert(Scanlines.end(), Thumbnail.Pixels.begin() + static_cast<ptrdiff_t>(Y * RowBytes),
					Thumbnail.Pixels.begin() + static_cast<ptrdiff_t>((Y + 1) * RowBytes));
			}

			std::vector<uint8> Deflate{0x78, 0x01};
			for (size_t Offset = 0; Offset < Scanlines.size();)
			{
				const uint16 BlockSize = static_cast<uint16>(std::min<size_t>(65'535, Scanlines.size() - Offset));
				const bool bFinal = Offset + BlockSize == Scanlines.size();
				Deflate.push_back(bFinal ? 1 : 0);
				Deflate.push_back(static_cast<uint8>(BlockSize));
				Deflate.push_back(static_cast<uint8>(BlockSize >> 8));
				const uint16 Inverse = static_cast<uint16>(~BlockSize);
				Deflate.push_back(static_cast<uint8>(Inverse));
				Deflate.push_back(static_cast<uint8>(Inverse >> 8));
				Deflate.insert(Deflate.end(), Scanlines.begin() + static_cast<ptrdiff_t>(Offset),
					Scanlines.begin() + static_cast<ptrdiff_t>(Offset + BlockSize));
				Offset += BlockSize;
			}
			uint32 S1 = 1;
			uint32 S2 = 0;
			for (const uint8 Byte : Scanlines)
			{
				S1 = (S1 + Byte) % 65'521;
				S2 = (S2 + S1) % 65'521;
			}
			AppendBigEndian(Deflate, (S2 << 16) | S1);

			OutBytes = {137, 80, 78, 71, 13, 10, 26, 10};
			std::vector<uint8> Header;
			AppendBigEndian(Header, Thumbnail.Width);
			AppendBigEndian(Header, Thumbnail.Height);
			Header.insert(Header.end(), {8, 6, 0, 0, 0});
			WritePngChunk(OutBytes, "IHDR", Header);
			WritePngChunk(OutBytes, "IDAT", Deflate);
			WritePngChunk(OutBytes, "IEND", {});
			return true;
		}

		auto DecodeCachedPng(std::span<const uint8> Bytes, uint32 MaximumDimension,
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
				if (OutThumbnail.Pixels[Index] != 255)
				{
					OutThumbnail.bHasTransparency = true;
					break;
				}
			return true;
		}

		auto MakeKey(const FSourceCacheKeyData& Entry) -> std::string
		{
			DerivedDataCache::FWriter Writer;
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
		::Durin::Editor::FAssetThumbnailObjectStore ObjectStore;
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
			.SourceTimeTicks = DerivedDataCache::FileTimeToStableTicks(EffectiveLastWriteTime),
			.MaximumDimension = Impl->Settings.MaximumDimension,
			.GeneratorVersion = Impl->Settings.GeneratorVersion,
			.ColorSpacePolicy = Impl->Settings.ColorSpacePolicy,
			.OutputEncodingVersion = Impl->Settings.OutputEncodingVersion,
		};
		Desired.Key = MakeKey(Desired);
		if (!Desired.SourceIdentity.empty())
		{
			std::vector<uint8> EncodedBytes;
			if (Impl->ObjectStore.Load(Desired.Key, EncodedBytes) == ::Durin::Editor::EAssetThumbnailObjectLoadResult::Hit)
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

		std::vector<uint8> EncodedBytes;
		if (!EncodeRgbaPng(OutThumbnail, EncodedBytes)) return true;
		Impl->ObjectStore.Store(Desired.Key, EncodedBytes);
		return true;
	}

	auto FSourceImageThumbnailDiskCache::GetStats() const -> FSourceImageThumbnailDiskCacheStats
	{
		const ::Durin::Editor::FAssetThumbnailObjectStoreStats StoreStats = Impl->ObjectStore.GetStats();
		std::lock_guard Lock(Impl->Mutex);
		return {
			.CacheHits = StoreStats.CacheHits,
			.SourceDecodes = Impl->SourceDecodes,
			.Regenerations = StoreStats.Regenerations};
	}
} // namespace Durin::Editor::Level
