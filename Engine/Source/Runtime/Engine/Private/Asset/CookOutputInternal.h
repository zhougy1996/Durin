#pragma once
#include "Asset/Cook.h"
namespace Durin::AssetPrivate
{
	inline constexpr uint32 CookStateMagic = 0x54414e53; // SNAT
	inline constexpr uint32 CookStateVersion = 2;
	inline constexpr uint64 MaximumCookStateBytes = 256ull * 1024 * 1024;
	inline constexpr uint32 MaximumCookStateEntries = 1'000'000;
	inline constexpr uint8 CookStateSegmentRawFieldProjection = 1 << 0;
	inline constexpr uint8 CookStateSegmentOpaque = 1 << 1;

	inline auto MakeCookStateEntry(const FCookSavePlan& Plan) -> FCookStateEntry
	{
		FCookStateEntry Entry{Plan.VirtualPath, Plan.InputFingerprint, Plan.PackageDigest,
			Plan.SegmentDigest, Plan.PackageFileSize, Plan.SegmentFileSize,
			Plan.ContributorVersion, Plan.FamilyProducerVersion, Plan.Contributor, Plan.BuildProvenance};
		Entry.SegmentFlags = static_cast<uint8>(
			(Plan.bRawBulkSegment ? CookStateSegmentRawFieldProjection : 0)
			| (Plan.bOpaqueRawSegment ? CookStateSegmentOpaque : 0));
		return Entry;
	}

	inline auto CookFail(std::string Message, std::string* OutError) -> bool
	{
		if (OutError) *OutError = std::move(Message);
		return false;
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
