#pragma once

#include "CoreMinimal.h"
#include "Asset/Cook.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageWriter.h"
namespace Durin::AssetPrivate
{
	inline constexpr std::string_view CookPackageSerializer = "engine-package";
	inline constexpr uint32 CookPackageSerializerVersion = 4;
	inline constexpr uint32 CookStateMagic = 0x54414e53; // SNAT
	inline constexpr uint32 CookStateVersion = 5;
	inline constexpr uint64 MaximumCookStateBytes = 256ull * 1024 * 1024;
	inline constexpr uint32 MaximumCookStateEntries = 1'000'000;
	inline constexpr uint8 CookStateSegmentRawFieldProjection = 1 << 0;
	inline constexpr uint8 CookStateSegmentOpaque = 1 << 1;

	// Decoded manifests have unique, sorted paths. Reuse references must identify
	// an artifact accepted by a prior publication, including its kind and flags.
	inline auto MatchesCookManifestOutput(const FCookManifest& Manifest, std::string_view Path,
		ECookManifestEntryKind Kind, uint8 Flags, uint64 Size, const FXxHash128& Digest) -> bool
	{
		const auto Entry = std::ranges::lower_bound(Manifest.Entries, Path, {}, &FCookManifestEntry::RelativePath);
		return Entry != Manifest.Entries.end() && Entry->RelativePath == Path && Entry->Kind == Kind
			&& Entry->Flags == Flags && Entry->FileSize == Size
			&& Entry->HashLow == Digest.HashLow && Entry->HashHigh == Digest.HashHigh;
	}

	// Bounded streaming verification, also used at the publication boundary. A
	// discovery hit is advisory: output can disappear before the writer lock.
	inline auto ValidateCookOutputFile(const std::filesystem::path& Path, uint64 Size,
		const FXxHash128& Digest, const std::function<bool()>& Continue = {}) -> bool
	{
		std::error_code Error;
		if (!std::filesystem::is_regular_file(Path, Error) || Error) return false;
		const std::array Paths{Path};
		auto Access = FPackageFileAccess::TryAcquire(Paths, false);
		if (!Access) return false;
		auto File = FFileHelper::OpenRead(Path);
		if (!File || (*File)->GetSize() != Size) return false;
		FByteBuffer Buffer(static_cast<size_t>(std::min<uint64>(Size, 1024 * 1024)));
		FXxHash128Builder Hash;
		for (uint64 Offset = 0; Offset < Size; Offset += Buffer.size())
		{
			const auto Chunk = std::span(Buffer).first(static_cast<size_t>(std::min<uint64>(Buffer.size(), Size - Offset)));
			if ((Continue && !Continue()) || !(*File)->ReadAt(Offset, Chunk)) return false;
			Hash.Update(Chunk);
		}
		return (!Continue || Continue()) && Hash.Finalize() == Digest;
	}

	inline auto MakeCookStateEntry(const FCookSavePlan& Plan) -> FCookStateEntry
	{
		FCookStateEntry Entry{Plan.VirtualPath, Plan.InputFingerprint, Plan.PackageDigest,
			Plan.SegmentDigest, Plan.PackageFileSize, Plan.SegmentFileSize,
			Plan.SerializerVersion, Plan.Serializer, Plan.BuildProvenance};
		Entry.SegmentFlags = static_cast<uint8>(
			(Plan.bRawBulkSegment ? CookStateSegmentRawFieldProjection : 0)
			| (Plan.bOpaqueRawSegment ? CookStateSegmentOpaque : 0));
		return Entry;
	}

	inline auto RelativePackagePath(std::string_view VirtualPath) -> std::string
	{
		return std::format("{}.dasset", VirtualPath.substr(1));
	}

	inline auto RelativeSegmentPath(std::string_view VirtualPath) -> std::string
	{
		return std::format("{}.dbulk", VirtualPath.substr(1));
	}
}
