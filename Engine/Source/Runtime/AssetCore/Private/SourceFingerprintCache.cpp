#include "SourceFingerprintCache.h"

#include "Misc/DerivedDataCache.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace Durin::Asset
{
	namespace
	{
		constexpr uint32 SourceFingerprintIndexMagic = 0x58494653; // SFIX
		constexpr uint32 SourceFingerprintIndexSchemaVersion = 1;
		constexpr uint32 SourceFingerprintIndexFormatVersion = 1;
		constexpr uint32 MaximumSourceFingerprintEntries = 1'000'000;
		constexpr uint64 MaximumSourceFingerprintIndexBytes = 64ull * 1024ull * 1024ull;
		constexpr uint64 MaximumSourceIdentityBytes = 32ull * 1024ull;
		constexpr uint64 MaximumContentHashBytes = 1024;

		struct FSourceFingerprintCacheState
		{
			std::mutex Mutex;
			std::filesystem::path LoadedRoot;
			std::unordered_map<std::string, FSourceFingerprint> Entries;
		};

		auto GetState() -> FSourceFingerprintCacheState&
		{
			static FSourceFingerprintCacheState State;
			return State;
		}

		auto GetCacheRoot() -> std::filesystem::path
		{
			return (std::filesystem::path(FPaths::DerivedDataCacheDir()) / "SourceFingerprints").lexically_normal();
		}

		auto GetSourceIdentity(const std::filesystem::path& SourcePath) -> std::string
		{
			std::error_code Error;
			std::filesystem::path Identity = std::filesystem::absolute(SourcePath, Error);
			if (Error) Identity = SourcePath;
			return Identity.lexically_normal().generic_string();
		}

		auto LoadIndex(FSourceFingerprintCacheState& State, const std::filesystem::path& Root) -> void
		{
			State.LoadedRoot = Root;
			State.Entries.clear();
			const std::filesystem::path IndexPath = Root / "Index.bin";
			std::error_code Error;
			if (!std::filesystem::is_regular_file(IndexPath, Error) || Error) return;
			const uint64 FileSize = std::filesystem::file_size(IndexPath, Error);
			if (Error || FileSize > MaximumSourceFingerprintIndexBytes) return;

			std::vector<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, IndexPath.generic_string())) return;
			DerivedDataCache::FReader Reader(Bytes);
			uint32 EntryCount = 0;
			if (!Reader.ReadAndValidateHeader(
					SourceFingerprintIndexMagic,
					SourceFingerprintIndexSchemaVersion,
					SourceFingerprintIndexFormatVersion)
				|| !Reader.ReadU32(EntryCount)
				|| EntryCount > MaximumSourceFingerprintEntries)
				return;

			std::unordered_map<std::string, FSourceFingerprint> LoadedEntries;
			LoadedEntries.reserve(EntryCount);
			for (uint32 Index = 0; Index < EntryCount; ++Index)
			{
				std::string Identity;
				FSourceFingerprint Fingerprint;
				if (!Reader.ReadString(Identity, MaximumSourceIdentityBytes)
					|| !Reader.ReadU64(Fingerprint.FileSize)
					|| !Reader.ReadI64(Fingerprint.LastWriteTimeTicks)
					|| !Reader.ReadString(Fingerprint.ContentHash, MaximumContentHashBytes)
					|| Identity.empty()
					|| Fingerprint.ContentHash.empty())
					return;
				LoadedEntries.insert_or_assign(std::move(Identity), std::move(Fingerprint));
			}
			if (!Reader.IsAtEnd()) return;
			State.Entries = std::move(LoadedEntries);
		}

		auto EnsureLoaded(FSourceFingerprintCacheState& State) -> std::filesystem::path
		{
			const std::filesystem::path Root = GetCacheRoot();
			if (State.LoadedRoot != Root) LoadIndex(State, Root);
			return Root;
		}

		auto SaveIndex(
			const FSourceFingerprintCacheState& State,
			const std::filesystem::path& Root) -> bool
		{
			if (State.Entries.size() > MaximumSourceFingerprintEntries) return false;
			std::vector<std::pair<std::string, FSourceFingerprint>> SortedEntries(
				State.Entries.begin(), State.Entries.end());
			std::ranges::sort(SortedEntries, [](const auto& Left, const auto& Right) {
				return Left.first < Right.first;
			});

			DerivedDataCache::FWriter Writer;
			Writer.WriteHeader({
				SourceFingerprintIndexMagic,
				SourceFingerprintIndexSchemaVersion,
				SourceFingerprintIndexFormatVersion});
			Writer.WriteU32(static_cast<uint32>(SortedEntries.size()));
			for (const auto& [Identity, Fingerprint] : SortedEntries)
			{
				Writer.WriteString(Identity);
				Writer.WriteU64(Fingerprint.FileSize);
				Writer.WriteI64(Fingerprint.LastWriteTimeTicks);
				Writer.WriteString(Fingerprint.ContentHash);
			}
			if (Writer.GetBytes().size() > MaximumSourceFingerprintIndexBytes) return false;

			std::error_code Error;
			std::filesystem::create_directories(Root, Error);
			return !Error && DerivedDataCache::WriteFileAtomically(Root / "Index.bin", Writer.GetBytes());
		}
	}

	auto FindSourceFingerprint(
		const std::filesystem::path& SourcePath,
		uint64 FileSize,
		int64 LastWriteTimeTicks,
		std::string& OutContentHash) -> bool
	{
		FSourceFingerprintCacheState& State = GetState();
		const std::scoped_lock Lock(State.Mutex);
		EnsureLoaded(State);
		const auto It = State.Entries.find(GetSourceIdentity(SourcePath));
		if (It == State.Entries.end()
			|| It->second.FileSize != FileSize
			|| It->second.LastWriteTimeTicks != LastWriteTimeTicks)
			return false;
		OutContentHash = It->second.ContentHash;
		return true;
	}

	auto StoreSourceFingerprint(
		const std::filesystem::path& SourcePath,
		const FSourceFingerprint& Fingerprint) -> bool
	{
		if (Fingerprint.ContentHash.empty()) return false;
		FSourceFingerprintCacheState& State = GetState();
		const std::scoped_lock Lock(State.Mutex);
		const std::filesystem::path Root = EnsureLoaded(State);
		State.Entries.insert_or_assign(GetSourceIdentity(SourcePath), Fingerprint);
		return SaveIndex(State, Root);
	}
}
