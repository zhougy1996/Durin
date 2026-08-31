#pragma once

#include "EngineAPI.h"

namespace Durin::Asset
{
	inline constexpr uint32 ChunkedPayloadHeaderSize = 64;
	inline constexpr uint32 ChunkedPayloadEntrySize = 32;
	inline constexpr uint32 ChunkedPayloadRequiredFlag = 1;

	enum class EChunkedPayloadFailure
	{
		None,
		InvalidArgument,
		TruncatedHeader,
		InvalidHeader,
		InvalidChunkCount,
		InvalidStoredSize,
		ChecksumMismatch,
		TableOutOfRange,
		DuplicateChunkType,
		UnsupportedChunkFlags,
		UnsupportedCompression,
		InvalidChunkLayout,
		InvalidChunkSize,
		CompressionRatioExceeded,
		NonzeroPadding,
		UnknownRequiredChunk,
		MissingRequiredChunk,
		DecodedSizeMismatch,
		CompressionFlagsMismatch,
		TrailingData,
		CompressionUnavailable
	};

	enum class EChunkedPayloadFailureKind
	{
		Corrupt,
		Incompatible
	};

	struct FChunkedPayloadFormat
	{
		uint32 HeaderSizeWordIndex = 0;
		uint32 ChunkCountWordIndex = 0;
		uint32 GlobalFlagsWordIndex = std::numeric_limits<uint32>::max();
		uint32 GlobalCompressedFlag = 0;
		uint32 RequiredChunkCount = 0;
		uint32 MaximumChunkCount = 0;
		uint32 RequiredChunkFlag = ChunkedPayloadRequiredFlag;
		uint32 KnownChunkFlags = ChunkedPayloadRequiredFlag;
		uint32 CompressionMask = 0;
		uint32 CompressionShift = 0;
		uint32 MaximumCompressionMethod = 0;
		uint64 MaximumCompressionRatio = 0;
		uint64 MaximumBytes = 0;
		uint64 Alignment = 1;
		bool RequireUniqueChunkTypes = false;
		bool RequireNonemptyRequiredChunks = false;
		bool AllowTrailingZeroPadding = false;
	};

	struct FChunkedPayloadInput
	{
		uint32 Type = 0;
		uint32 Flags = 0;
		std::span<const std::byte> Bytes;
		uint64 DecodedSize = 0;
	};

	struct FChunkedPayloadView
	{
		uint32 Type = 0;
		uint32 Flags = 0;
		std::span<const std::byte> Bytes;
		uint64 DecodedSize = 0;
	};

	struct FDecodedChunkedPayload
	{
		std::array<uint32, 8> HeaderWords{};
		std::vector<FChunkedPayloadView> Chunks;
		std::vector<std::span<const std::byte>> RequiredChunks;
	};

	struct FChunkedPayloadResult
	{
		EChunkedPayloadFailure Failure = EChunkedPayloadFailure::None;
		EChunkedPayloadFailureKind Kind = EChunkedPayloadFailureKind::Corrupt;

		explicit operator bool() const { return Failure == EChunkedPayloadFailure::None; }
	};

	// Encodes the shared 64-byte header and 32-byte chunk-directory envelope.
	// Format-specific callers own the first eight header words and chunk contents.
	ENGINE_API auto EncodeChunkedPayload(
		std::array<uint32, 8> HeaderWords,
		std::span<const FChunkedPayloadInput> Chunks,
		const FChunkedPayloadFormat& Format,
		FByteArray& OutBytes) -> FChunkedPayloadResult;

	// Validates and projects an envelope without interpreting chunk contents.
	ENGINE_API auto DecodeChunkedPayload(
		std::span<const std::byte> Bytes,
		const FChunkedPayloadFormat& Format,
		FDecodedChunkedPayload& OutPayload) -> FChunkedPayloadResult;

	ENGINE_API auto DescribeChunkedPayloadFailure(
		EChunkedPayloadFailure Failure,
		std::string_view PayloadName) -> std::string;
}
