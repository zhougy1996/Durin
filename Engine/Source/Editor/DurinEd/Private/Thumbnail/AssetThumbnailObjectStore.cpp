#include "Thumbnail/AssetThumbnailObjectStore.h"

#include "Misc/FileHelper.h"
#include "Misc/LexicalPath.h"
#include "Misc/Paths.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::Editor
{
	namespace
	{
		constexpr uint32 ThumbnailIndexMagic = 0x58444954; // TIDX
		constexpr uint32 ThumbnailIndexSchemaVersion = 2;
		constexpr uint32 MaximumIndexEntries = 1'000'000;

		struct FObjectIndexEntry
		{
			std::string Key;
			uint64 EncodedBytes = 0;
			uint64 LastAccess = 0;
		};

		auto IsSafeKey(std::string_view Key) -> bool
		{
			return Key.size() >= 2 && Key.size() <= 128
				&& std::ranges::all_of(Key, [](char Character) {
					return (Character >= '0' && Character <= '9')
						|| (Character >= 'a' && Character <= 'z')
						|| (Character >= 'A' && Character <= 'Z')
						|| Character == '-' || Character == '_';
				});
		}

		auto TryResolveContainedBy(const std::filesystem::path& Root,
			const std::filesystem::path& Candidate,
			std::filesystem::path& OutResolved) -> bool
		{
			std::error_code Error;
			return PathUtilities::TryResolveContainedPath(
				Candidate, Root, OutResolved, Error);
		}
	} // namespace

	struct FAssetThumbnailObjectStore::FImpl
	{
		explicit FImpl(FAssetThumbnailObjectStoreSettings InSettings)
			: Settings(std::move(InSettings))
		{
			if (Settings.CacheRoot.empty())
				Settings.CacheRoot = std::filesystem::path(FPaths::DerivedDataCacheDir()) / "Thumbnails";
			Settings.CacheRoot = Settings.CacheRoot.lexically_normal();
			LoadIndex();
			bool bChanged = false;
			for (auto It = Entries.begin(); It != Entries.end();)
			{
				std::error_code Error;
				const std::filesystem::path Path = ObjectPath(It->first);
				std::filesystem::path ResolvedPath;
				const bool bContained = TryResolveContainedBy(Settings.CacheRoot, Path, ResolvedPath);
				const uintmax_t Size = bContained
					? std::filesystem::file_size(ResolvedPath, Error) : 0;
				if (Error || Size != It->second.EncodedBytes || Size > Settings.MaximumObjectBytes
					|| !bContained)
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

		FAssetThumbnailObjectStoreSettings Settings;
		std::unordered_map<std::string, FObjectIndexEntry> Entries;
		FAssetThumbnailObjectStoreStats Stats;
		uint64 AccessCounter = 0;
		mutable std::mutex Mutex;

		auto IndexPath() const -> std::filesystem::path { return Settings.CacheRoot / "Index.bin"; }
		auto ObjectPath(std::string_view Key) const -> std::filesystem::path
		{
			return Settings.CacheRoot / "Objects" / std::string(Key.substr(0, 2))
				/ (std::string(Key) + Settings.ObjectExtension);
		}

		auto LoadIndex() -> void
		{
			std::vector<std::byte> Bytes;
			std::error_code Error;
			if (!std::filesystem::is_regular_file(IndexPath(), Error)
				|| !FFileHelper::LoadFileToArray(Bytes, IndexPath()))
				return;
			FBinaryReader Reader(Bytes);
			uint32 Count = 0;
			if (!Reader.ReadAndValidateHeader(ThumbnailIndexMagic,
					ThumbnailIndexSchemaVersion, Settings.FormatVersion)
				|| !Reader.ReadU32(Count) || Count > MaximumIndexEntries)
				return;
			std::unordered_map<std::string, FObjectIndexEntry> Loaded;
			for (uint32 Index = 0; Index < Count; ++Index)
			{
				FObjectIndexEntry Entry;
				if (!Reader.ReadString(Entry.Key, 128) || !Reader.ReadU64(Entry.EncodedBytes)
					|| !Reader.ReadU64(Entry.LastAccess) || !IsSafeKey(Entry.Key)
					|| Entry.EncodedBytes > Settings.MaximumObjectBytes
					|| !Loaded.emplace(Entry.Key, Entry).second)
					return;
				AccessCounter = std::max(AccessCounter, Entry.LastAccess);
			}
			if (Reader.IsAtEnd()) Entries = std::move(Loaded);
		}

		auto SaveIndex() -> void
		{
			std::vector<FObjectIndexEntry> Sorted;
			Sorted.reserve(Entries.size());
			for (const auto& [Key, Entry] : Entries) Sorted.push_back(Entry);
			std::ranges::sort(Sorted, {}, &FObjectIndexEntry::Key);
			FBinaryWriter Writer;
			Writer.WriteHeader({ThumbnailIndexMagic,
				ThumbnailIndexSchemaVersion, Settings.FormatVersion});
			Writer.WriteU32(static_cast<uint32>(Sorted.size()));
			for (const FObjectIndexEntry& Entry : Sorted)
			{
				Writer.WriteString(Entry.Key);
				Writer.WriteU64(Entry.EncodedBytes);
				Writer.WriteU64(Entry.LastAccess);
			}
			std::error_code Error;
			std::filesystem::create_directories(Settings.CacheRoot, Error);
			if (!Error) FFileHelper::SaveArrayToFileAtomically(Writer.GetBytes(), IndexPath());
		}

		auto RemoveObject(const FObjectIndexEntry& Entry) -> void
		{
			const std::filesystem::path Path = ObjectPath(Entry.Key);
			std::filesystem::path ResolvedPath;
			if (!TryResolveContainedBy(Settings.CacheRoot, Path, ResolvedPath)) return;
			std::error_code Error;
			std::filesystem::remove(ResolvedPath, Error);
		}

		auto MaintainBudget() -> void
		{
			uint64 TotalBytes = 0;
			for (const auto& [Key, Entry] : Entries) TotalBytes += Entry.EncodedBytes;
			while (TotalBytes > Settings.DiskBudgetBytes && !Entries.empty())
			{
				const auto Candidate = std::ranges::min_element(Entries, {}, [](const auto& Pair) {
					return std::pair(Pair.second.LastAccess, Pair.first);
				});
				TotalBytes -= Candidate->second.EncodedBytes;
				RemoveObject(Candidate->second);
				Entries.erase(Candidate);
				++Stats.Evictions;
			}
		}
	};

	FAssetThumbnailObjectStore::FAssetThumbnailObjectStore(FAssetThumbnailObjectStoreSettings Settings)
		: Impl(std::make_unique<FImpl>(std::move(Settings)))
	{
	}

	FAssetThumbnailObjectStore::~FAssetThumbnailObjectStore() = default;

	auto FAssetThumbnailObjectStore::Load(std::string_view Key, std::vector<std::byte>& OutBytes)
		-> EAssetThumbnailObjectLoadResult
	{
		OutBytes.clear();
		if (!IsSafeKey(Key)) return EAssetThumbnailObjectLoadResult::Invalid;
		FObjectIndexEntry Entry;
		{
			std::lock_guard Lock(Impl->Mutex);
			const auto It = Impl->Entries.find(std::string(Key));
			if (It == Impl->Entries.end()) return EAssetThumbnailObjectLoadResult::Miss;
			Entry = It->second;
		}
		const std::filesystem::path Path = Impl->ObjectPath(Key);
		std::filesystem::path ResolvedPath;
		const bool bContained = TryResolveContainedBy(Impl->Settings.CacheRoot, Path, ResolvedPath);
		std::error_code Error;
		const uintmax_t EncodedSize = bContained
			? std::filesystem::file_size(ResolvedPath, Error) : 0;
		if (!Error && EncodedSize == Entry.EncodedBytes && EncodedSize <= Impl->Settings.MaximumObjectBytes
			&& bContained
			&& FFileHelper::LoadFileToArray(OutBytes, ResolvedPath)
			&& OutBytes.size() == EncodedSize)
		{
			std::lock_guard Lock(Impl->Mutex);
			if (auto It = Impl->Entries.find(std::string(Key)); It != Impl->Entries.end())
			{
				It->second.LastAccess = ++Impl->AccessCounter;
				++Impl->Stats.CacheHits;
				Impl->SaveIndex();
				return EAssetThumbnailObjectLoadResult::Hit;
			}
		}
		std::lock_guard Lock(Impl->Mutex);
		if (auto It = Impl->Entries.find(std::string(Key)); It != Impl->Entries.end())
		{
			Impl->RemoveObject(It->second);
			Impl->Entries.erase(It);
			++Impl->Stats.Regenerations;
			Impl->SaveIndex();
		}
		OutBytes.clear();
		return EAssetThumbnailObjectLoadResult::Invalid;
	}

	auto FAssetThumbnailObjectStore::Store(std::string_view Key, std::span<const std::byte> Bytes) -> bool
	{
		if (!IsSafeKey(Key) || Bytes.empty() || Bytes.size() > Impl->Settings.MaximumObjectBytes) return false;
		const std::filesystem::path ObjectPath = Impl->ObjectPath(Key);
		std::filesystem::path ResolvedObjectPath;
		if (!TryResolveContainedBy(Impl->Settings.CacheRoot, ObjectPath, ResolvedObjectPath)) return false;
		std::error_code Error;
		std::filesystem::create_directories(ObjectPath.parent_path(), Error);
		if (Error || !TryResolveContainedBy(
			Impl->Settings.CacheRoot, ObjectPath, ResolvedObjectPath)) return false;
		if (!FFileHelper::SaveArrayToFileAtomically(Bytes, ResolvedObjectPath)) return false;
		std::lock_guard Lock(Impl->Mutex);
		Impl->Entries.insert_or_assign(std::string(Key), FObjectIndexEntry{
			.Key = std::string(Key),
			.EncodedBytes = Bytes.size(),
			.LastAccess = ++Impl->AccessCounter});
		Impl->MaintainBudget();
		Impl->SaveIndex();
		return true;
	}

	auto FAssetThumbnailObjectStore::Invalidate(std::string_view Key) -> void
	{
		if (!IsSafeKey(Key)) return;
		std::lock_guard Lock(Impl->Mutex);
		if (auto It = Impl->Entries.find(std::string(Key)); It != Impl->Entries.end())
		{
			Impl->RemoveObject(It->second);
			Impl->Entries.erase(It);
			++Impl->Stats.Regenerations;
			Impl->SaveIndex();
		}
	}

	auto FAssetThumbnailObjectStore::GetStats() const -> FAssetThumbnailObjectStoreStats
	{
		std::lock_guard Lock(Impl->Mutex);
		return Impl->Stats;
	}

	auto SelectAssetThumbnailBudgetEvictions(
		std::span<const FAssetThumbnailBudgetEntry> Entries, uint64 BudgetBytes) -> std::vector<std::string>
	{
		uint64 ResidentBytes = 0;
		for (const FAssetThumbnailBudgetEntry& Entry : Entries) ResidentBytes += Entry.Bytes;
		std::vector<const FAssetThumbnailBudgetEntry*> Candidates;
		for (const FAssetThumbnailBudgetEntry& Entry : Entries)
			if (Entry.Bytes > 0 && !Entry.bPinned) Candidates.push_back(&Entry);
		std::ranges::sort(Candidates, [](const FAssetThumbnailBudgetEntry* A, const FAssetThumbnailBudgetEntry* B) {
			return std::pair(A->LastUsed, A->Key) < std::pair(B->LastUsed, B->Key);
		});
		std::vector<std::string> Evictions;
		for (const FAssetThumbnailBudgetEntry* Candidate : Candidates)
		{
			if (ResidentBytes <= BudgetBytes) break;
			ResidentBytes -= Candidate->Bytes;
			Evictions.push_back(Candidate->Key);
		}
		return Evictions;
	}
} // namespace Durin::Editor
