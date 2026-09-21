#pragma once

#include <expected>

#include "EngineAPI.h"
#include "Hash/XxHash.h"

namespace Durin
{
	inline constexpr uint64 PackageBulkDataMaximumSegmentBytes = 1024ull * 1024ull * 1024ull;
	inline constexpr uint64 PackageBulkDataMaximumFieldCount = 65'536;
	inline constexpr uint64 EditorBulkDataExternalThreshold = 256ull * 1024ull;
	inline constexpr uint32 EditorBulkDataExternalAlignment = 16;

	// Selects whether one authored field is carried by DAST or the raw package segment.
	enum class EPackageBulkDataPlacement : uint32
	{
		Inline = 0,
		External = 1,
	};

	// Binds one DAST generation to its optional raw package segment.
	struct FPackageBulkSegmentSummary
	{
		uint64 Extent = 0;
		FXxHash128 Digest;
		uint32 Flags = 0;

		auto operator==(const FPackageBulkSegmentSummary&) const -> bool = default;
	};

	// Mirrors the placement facts of one logical BulkData field for construct-free validation.
	struct FPackageBulkDataEntry
	{
		uint64 FieldIndex = 0;
		EPackageBulkDataPlacement Placement = EPackageBulkDataPlacement::Inline;
		uint32 StorageFlags = 0;
		uint64 LogicalSize = 0;
		uint64 StoredSize = 0;
		uint64 SegmentOffset = 0;
		uint32 Alignment = 1;
		FXxHash128 ContentId;

		auto operator==(const FPackageBulkDataEntry&) const -> bool = default;
	};

	enum class EPackageBulkDataError : uint8
	{
		None, UnsupportedFlags, SegmentLimit, DigestPresence, FieldLimit,
		NonCanonicalIndex, UnsupportedStorage, MissingContentIdentity,
		InvalidInlineMetadata, InvalidPlacement, InvalidExternalMetadata,
		RangeOutsideSegment, InvalidFinalExtent, ExtentMismatch, SegmentDigestMismatch,
		NonzeroPadding, FieldDigestMismatch,
	};
	struct FPackageBulkDataError
	{
		EPackageBulkDataError Code = EPackageBulkDataError::None;
		uint64 Index = 0;
		FPackageBulkSegmentSummary Summary;
		std::optional<FPackageBulkDataEntry> Entry;
		uint64 Actual = 0;
		uint64 Expected = 0;
		uint64 Offset = 0;
		FXxHash128 ActualDigest{};
	};
	using FPackageBulkDataResult = std::expected<void, FPackageBulkDataError>;
	ENGINE_API auto FormatPackageBulkDataError(const FPackageBulkDataError& Error) -> std::string;

	[[nodiscard]] ENGINE_API auto ValidatePackageBulkDataMetadata(
		const FPackageBulkSegmentSummary& Summary,
		std::span<const FPackageBulkDataEntry> Entries) -> FPackageBulkDataResult;

	// Validates exact extent, digest, declared payload ranges, and zero padding.
	[[nodiscard]] ENGINE_API auto ValidatePackageBulkDataSegment(
		const FPackageBulkSegmentSummary& Summary,
		std::span<const FPackageBulkDataEntry> Entries,
		FByteView Segment) -> FPackageBulkDataResult;
}
