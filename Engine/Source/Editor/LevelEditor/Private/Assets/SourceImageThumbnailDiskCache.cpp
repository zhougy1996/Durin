#include "Assets/SourceImageThumbnailDiskCache.h"

#include "Hash/XxHash.h"
#include "ImageDecoder.h"
#include "Misc/DerivedDataCache.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 MaximumIndexEntries = 1'000'000;
		constexpr uint64 MaximumEncodedObjectBytes = 16ull * 1024ull * 1024ull;

		struct FIndexEntry
		{
			std::string Key;
			std::string SourceIdentity;
			uint64 SourceSize = 0;
			int64 SourceTimeTicks = 0;
			uint32 MaximumDimension = 0;
			uint32 GeneratorVersion = 0;
			uint32 ColorSpacePolicy = 0;
			uint32 OutputEncodingVersion = 0;
			uint64 EncodedBytes = 0;
			uint64 LastAccess = 0;
		};

		auto IsContainedBy(const std::filesystem::path& Root, const std::filesystem::path& Candidate) -> bool
		{
			const std::filesystem::path Relative = Candidate.lexically_normal().lexically_relative(Root.lexically_normal());
			if (Relative.empty() || Relative.is_absolute()) return false;
			for (const auto& Part : Relative)
				if (Part == "..") return false;
			return true;
		}

		auto IsResolvedContainedBy(const std::filesystem::path& Root, const std::filesystem::path& Candidate) -> bool
		{
			std::error_code Error;
			const std::filesystem::path ResolvedRoot = std::filesystem::weakly_canonical(Root, Error);
			if (Error) return false;
			const std::filesystem::path ResolvedCandidate = std::filesystem::weakly_canonical(Candidate, Error);
			return !Error && IsContainedBy(ResolvedRoot, ResolvedCandidate);
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
			for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
			{
				const std::filesystem::path Root = std::filesystem::weakly_canonical(Mount.PhysicalPath, Error);
				if (Error || !IsContainedBy(Root, Absolute)) continue;
				return std::filesystem::path(Mount.VirtualRoot).append(Absolute.lexically_relative(Root).generic_string()).lexically_normal().generic_string();
			}

			const std::array<std::pair<std::string_view, std::string>, 2> Roots{{
				{"$Project/", FPaths::ProjectDir()},
				{"$Engine/", FPaths::EngineDir()},
			}};
			for (const auto& [Prefix, RootString] : Roots)
			{
				if (RootString.empty()) continue;
				const std::filesystem::path Root = std::filesystem::weakly_canonical(RootString, Error);
				if (Error || !IsContainedBy(Root, Absolute)) continue;
				return std::string(Prefix) + Absolute.lexically_relative(Root).generic_string();
			}
			return {};
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

		auto DecodeCachedPng(const std::filesystem::path& Path, uint32 MaximumDimension, FDecodedSourceImageThumbnail& OutThumbnail, std::string& OutError) -> bool
		{
			Asset::FDecodedImage Image;
			if (!Asset::DecodeImageFromFile(Path.generic_string(), Image, OutError,
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

		auto MakeKey(const FIndexEntry& Entry) -> std::string
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

	struct FSourceImageThumbnailDiskCache::FImpl
	{
		explicit FImpl(FSourceImageThumbnailDiskCacheSettings InSettings)
			: Settings(std::move(InSettings))
		{
			if (Settings.CacheRoot.empty()) Settings.CacheRoot = std::filesystem::path(FPaths::DerivedDataCacheDir()) / "Thumbnails";
			Settings.CacheRoot = Settings.CacheRoot.lexically_normal();
			LoadIndex();
			bool bChanged = false;
			for (auto It = Entries.begin(); It != Entries.end();)
			{
				std::error_code Error;
				const std::filesystem::path Path = ObjectPath(It->first);
				const uintmax_t Size = std::filesystem::file_size(Path, Error);
				if (Error || Size != It->second.EncodedBytes || Size > MaximumEncodedObjectBytes || !IsResolvedContainedBy(Settings.CacheRoot, Path))
				{
					It = Entries.erase(It);
					++Stats.Regenerations;
					bChanged = true;
				}
				else
					++It;
			}
			const size_t PreviousCount = Entries.size();
			MaintainBudget();
			if (bChanged || Entries.size() != PreviousCount) SaveIndex();
		}

		FSourceImageThumbnailDiskCacheSettings Settings;
		std::unordered_map<std::string, FIndexEntry> Entries;
		FSourceImageThumbnailDiskCacheStats Stats;
		uint64 AccessCounter = 0;
		mutable std::mutex Mutex;

		auto IndexPath() const -> std::filesystem::path { return Settings.CacheRoot / "Index.bin"; }
		auto ObjectPath(std::string_view Key) const -> std::filesystem::path
		{
			return Settings.CacheRoot / "Objects" / std::string(Key.substr(0, 2)) / (std::string(Key) + ".png");
		}

		auto LoadIndex() -> void
		{
			std::vector<uint8> Bytes;
			std::error_code Error;
			if (!std::filesystem::is_regular_file(IndexPath(), Error)
				|| !FFileHelper::LoadFileToArray(Bytes, IndexPath().generic_string()))
				return;
			DerivedDataCache::FReader Reader(Bytes);
			uint32 Count = 0;
			if (!Reader.ReadAndValidateHeader(DerivedDataCache::ThumbnailIndexMagic, DerivedDataCache::ThumbnailIndexSchemaVersion,
					Settings.OutputEncodingVersion)
				|| !Reader.ReadU32(Count) || Count > MaximumIndexEntries)
				return;
			std::unordered_map<std::string, FIndexEntry> Loaded;
			for (uint32 Index = 0; Index < Count; ++Index)
			{
				FIndexEntry Entry;
				if (!Reader.ReadString(Entry.Key, 64) || !Reader.ReadString(Entry.SourceIdentity)
					|| !Reader.ReadU64(Entry.SourceSize) || !Reader.ReadI64(Entry.SourceTimeTicks)
					|| !Reader.ReadU32(Entry.MaximumDimension) || !Reader.ReadU32(Entry.GeneratorVersion)
					|| !Reader.ReadU32(Entry.ColorSpacePolicy)
					|| !Reader.ReadU32(Entry.OutputEncodingVersion) || !Reader.ReadU64(Entry.EncodedBytes)
					|| !Reader.ReadU64(Entry.LastAccess) || Entry.Key != MakeKey(Entry) || !Loaded.emplace(Entry.Key, Entry).second)
					return;
				AccessCounter = std::max(AccessCounter, Entry.LastAccess);
			}
			if (Reader.IsAtEnd()) Entries = std::move(Loaded);
		}

		auto SaveIndex() -> void
		{
			std::vector<FIndexEntry> Sorted;
			Sorted.reserve(Entries.size());
			for (const auto& [Key, Entry] : Entries) Sorted.push_back(Entry);
			std::ranges::sort(Sorted, {}, &FIndexEntry::Key);
			DerivedDataCache::FWriter Writer;
			Writer.WriteHeader({DerivedDataCache::ThumbnailIndexMagic, DerivedDataCache::ThumbnailIndexSchemaVersion, Settings.OutputEncodingVersion});
			Writer.WriteU32(static_cast<uint32>(Sorted.size()));
			for (const FIndexEntry& Entry : Sorted)
			{
				Writer.WriteString(Entry.Key);
				Writer.WriteString(Entry.SourceIdentity);
				Writer.WriteU64(Entry.SourceSize);
				Writer.WriteI64(Entry.SourceTimeTicks);
				Writer.WriteU32(Entry.MaximumDimension);
				Writer.WriteU32(Entry.GeneratorVersion);
				Writer.WriteU32(Entry.ColorSpacePolicy);
				Writer.WriteU32(Entry.OutputEncodingVersion);
				Writer.WriteU64(Entry.EncodedBytes);
				Writer.WriteU64(Entry.LastAccess);
			}
			std::error_code Error;
			std::filesystem::create_directories(Settings.CacheRoot, Error);
			if (!Error) DerivedDataCache::WriteFileAtomically(IndexPath(), Writer.GetBytes());
		}

		auto RemoveObject(const FIndexEntry& Entry) -> void
		{
			const std::filesystem::path Path = ObjectPath(Entry.Key);
			if (IsResolvedContainedBy(Settings.CacheRoot, Path))
			{
				std::error_code Error;
				std::filesystem::remove(Path, Error);
			}
		}

		auto MaintainBudget() -> void
		{
			uint64 TotalBytes = 0;
			for (const auto& [Key, Entry] : Entries) TotalBytes += Entry.EncodedBytes;
			while (TotalBytes > Settings.DiskBudgetBytes && !Entries.empty())
			{
				const auto Candidate = std::ranges::min_element(Entries, {}, [](const auto& Pair) { return Pair.second.LastAccess; });
				TotalBytes -= Candidate->second.EncodedBytes;
				RemoveObject(Candidate->second);
				Entries.erase(Candidate);
			}
		}
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
		FIndexEntry Desired{
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
			FIndexEntry CachedEntry;
			bool bHasCachedEntry = false;
			{
				std::lock_guard Lock(Impl->Mutex);
				if (const auto It = Impl->Entries.find(Desired.Key); It != Impl->Entries.end())
				{
					CachedEntry = It->second;
					bHasCachedEntry = true;
				}
			}
			if (bHasCachedEntry)
			{
				const std::filesystem::path Path = Impl->ObjectPath(Desired.Key);
				std::error_code Error;
				const uintmax_t EncodedSize = std::filesystem::file_size(Path, Error);
				if (!Error && EncodedSize == CachedEntry.EncodedBytes && EncodedSize <= MaximumEncodedObjectBytes
					&& IsResolvedContainedBy(Impl->Settings.CacheRoot, Path)
					&& DecodeCachedPng(Path, Impl->Settings.MaximumDimension, OutThumbnail, OutError))
				{
					std::lock_guard Lock(Impl->Mutex);
					if (auto It = Impl->Entries.find(Desired.Key); It != Impl->Entries.end())
					{
						It->second.LastAccess = ++Impl->AccessCounter;
						++Impl->Stats.CacheHits;
						Impl->SaveIndex();
						return true;
					}
				}
				std::lock_guard Lock(Impl->Mutex);
				if (auto It = Impl->Entries.find(Desired.Key); It != Impl->Entries.end())
				{
					Impl->RemoveObject(It->second);
					Impl->Entries.erase(It);
					++Impl->Stats.Regenerations;
				}
			}
		}

		{
			std::lock_guard Lock(Impl->Mutex);
			++Impl->Stats.SourceDecodes;
		}
		if (!DecodeSourceImageThumbnail(PhysicalPath, Impl->Settings.MaximumDimension, OutThumbnail, OutError)) return false;
		if (Desired.SourceIdentity.empty()) return true;

		std::vector<uint8> EncodedBytes;
		if (!EncodeRgbaPng(OutThumbnail, EncodedBytes)) return true;
		const std::filesystem::path ObjectPath = Impl->ObjectPath(Desired.Key);
		std::error_code DirectoryError;
		std::filesystem::create_directories(ObjectPath.parent_path(), DirectoryError);
		if (DirectoryError || !IsResolvedContainedBy(Impl->Settings.CacheRoot, ObjectPath)) return true;
		std::string CacheError;
		if (!DerivedDataCache::WriteFileAtomically(ObjectPath, EncodedBytes, &CacheError)) return true;
		Desired.EncodedBytes = EncodedBytes.size();
		{
			std::lock_guard Lock(Impl->Mutex);
			Desired.LastAccess = ++Impl->AccessCounter;
			Impl->Entries.insert_or_assign(Desired.Key, Desired);
			Impl->MaintainBudget();
			Impl->SaveIndex();
		}
		return true;
	}

	auto FSourceImageThumbnailDiskCache::GetStats() const -> FSourceImageThumbnailDiskCacheStats
	{
		std::lock_guard Lock(Impl->Mutex);
		return Impl->Stats;
	}
} // namespace Durin
