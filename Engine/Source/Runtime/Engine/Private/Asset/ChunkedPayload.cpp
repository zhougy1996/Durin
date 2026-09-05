#include "Asset/ChunkedPayload.h"

#include "BulkContainerInfrastructure.h"
#include "Hash/XxHash.h"

namespace Durin
{
	namespace
	{
		struct FChunkRecord
		{
			uint32 Type = 0;
			uint32 Flags = 0;
			uint64 Offset = 0;
			uint64 StoredSize = 0;
			uint64 DecodedSize = 0;
		};

		auto ChunkedPayloadFail(
			EChunkedPayloadFailure Failure,
			EChunkedPayloadFailureKind Kind = EChunkedPayloadFailureKind::Corrupt)
			-> FChunkedPayloadResult
		{
			return {Failure, Kind};
		}

		auto IsFormatValid(const FChunkedPayloadFormat& Format) -> bool
		{
			return Format.HeaderSizeWordIndex < 8
				&& Format.ChunkCountWordIndex < 8
				&& (Format.GlobalFlagsWordIndex < 8
					|| Format.GlobalFlagsWordIndex == std::numeric_limits<uint32>::max())
				&& Format.MaximumChunkCount >= Format.RequiredChunkCount
				&& Format.MaximumBytes >= ChunkedPayloadHeaderSize
				&& BulkContainer::IsPowerOfTwo(Format.Alignment)
				&& Format.RequiredChunkFlag != 0
				&& (Format.KnownChunkFlags & Format.RequiredChunkFlag) != 0
				&& (Format.CompressionMask == 0 || Format.CompressionShift < 32);
		}

		auto ReadHeader(
			BulkContainer::FBoundedReader& Reader,
			std::array<uint32, 8>& OutWords,
			uint64& OutTableOffset,
			uint64& OutDecodedSize,
			uint64& OutStoredSize,
			uint64& OutHash) -> bool
		{
			for (uint32& Word : OutWords)
				if (!Reader.Read(Word)) return false;
			return Reader.Read(OutTableOffset)
				&& Reader.Read(OutDecodedSize)
				&& Reader.Read(OutStoredSize)
				&& Reader.Read(OutHash);
		}
	}

	auto EncodeChunkedPayload(
		std::array<uint32, 8> HeaderWords,
		std::span<const FChunkedPayloadInput> Chunks,
		const FChunkedPayloadFormat& Format,
		FByteBuffer& OutBytes) -> FChunkedPayloadResult
	{
		if (!IsFormatValid(Format) || Chunks.size() < Format.RequiredChunkCount
			|| Chunks.size() > Format.MaximumChunkCount)
			return ChunkedPayloadFail(EChunkedPayloadFailure::InvalidArgument);

		uint64 DirectoryBytes = 0, DirectoryEnd = 0;
		if (!BulkContainer::TryMultiply(
			Chunks.size(), ChunkedPayloadEntrySize, Format.MaximumBytes, DirectoryBytes)
			|| !BulkContainer::TryAdd(
				ChunkedPayloadHeaderSize, DirectoryBytes, Format.MaximumBytes, DirectoryEnd))
			return ChunkedPayloadFail(EChunkedPayloadFailure::InvalidChunkCount);

		std::vector<BulkContainer::FLayoutItem> Items;
		Items.reserve(Chunks.size());
		uint64 TotalDecodedSize = 0;
		for (const FChunkedPayloadInput& Chunk : Chunks)
		{
			if (Chunk.Type == 0 || Chunk.DecodedSize > Format.MaximumBytes - TotalDecodedSize)
				return ChunkedPayloadFail(EChunkedPayloadFailure::InvalidChunkSize);
			Items.push_back({Chunk.Bytes.size(), Format.Alignment});
			TotalDecodedSize += Chunk.DecodedSize;
		}

		const BulkContainer::FLayoutPolicy LayoutPolicy{
			.MaximumCount = Format.MaximumChunkCount,
			.MaximumPayloadBytes = Format.MaximumBytes,
			.MaximumContainerBytes = Format.MaximumBytes};
		std::vector<BulkContainer::FPayloadRange> Ranges;
		uint64 StoredSize = 0;
		if (!BulkContainer::TryBuildLayout(
			DirectoryEnd, Items, LayoutPolicy, Ranges, StoredSize))
			return ChunkedPayloadFail(EChunkedPayloadFailure::InvalidChunkLayout);

		HeaderWords[Format.HeaderSizeWordIndex] = ChunkedPayloadHeaderSize;
		HeaderWords[Format.ChunkCountWordIndex] = static_cast<uint32>(Chunks.size());
		BulkContainer::FBoundedWriter Body(Format.MaximumBytes - ChunkedPayloadHeaderSize);
		for (size_t Index = 0; Index < Chunks.size(); ++Index)
		{
			const FChunkedPayloadInput& Chunk = Chunks[Index];
			const BulkContainer::FPayloadRange& Range = Ranges[Index];
			if (!Body.Write(Chunk.Type) || !Body.Write(Chunk.Flags)
				|| !Body.Write(Range.Offset) || !Body.Write(Range.Size)
				|| !Body.Write(Chunk.DecodedSize))
				return ChunkedPayloadFail(EChunkedPayloadFailure::InvalidStoredSize);
		}
		for (size_t Index = 0; Index < Chunks.size(); ++Index)
		{
			if (!Body.PadTo(Ranges[Index].Offset - ChunkedPayloadHeaderSize)
				|| !Body.Write(Chunks[Index].Bytes))
				return ChunkedPayloadFail(EChunkedPayloadFailure::InvalidStoredSize);
		}

		BulkContainer::FBoundedWriter Writer(Format.MaximumBytes);
		for (uint32 Word : HeaderWords)
			if (!Writer.Write(Word)) return ChunkedPayloadFail(EChunkedPayloadFailure::InvalidStoredSize);
		if (!Writer.Write(static_cast<uint64>(ChunkedPayloadHeaderSize))
			|| !Writer.Write(TotalDecodedSize) || !Writer.Write(StoredSize)
			|| !Writer.Write(FXxHash64::HashBuffer(Body.View()).HashValue)
			|| !Writer.Write(Body.View()) || !Writer.TryTake(OutBytes))
			return ChunkedPayloadFail(EChunkedPayloadFailure::InvalidStoredSize);
		return {};
	}

	auto DecodeChunkedPayload(
		FByteView Bytes,
		const FChunkedPayloadFormat& Format,
		FDecodedChunkedPayload& OutPayload) -> FChunkedPayloadResult
	{
		if (!IsFormatValid(Format)) return ChunkedPayloadFail(EChunkedPayloadFailure::InvalidArgument);
		if (Bytes.size() < ChunkedPayloadHeaderSize)
			return ChunkedPayloadFail(EChunkedPayloadFailure::TruncatedHeader);

		BulkContainer::FBoundedReader Reader(Bytes, Format.MaximumBytes);
		FDecodedChunkedPayload Candidate;
		uint64 TableOffset = 0, TotalDecodedSize = 0, StoredSize = 0, StoredHash = 0;
		if (!ReadHeader(Reader, Candidate.HeaderWords, TableOffset,
			TotalDecodedSize, StoredSize, StoredHash))
			return ChunkedPayloadFail(EChunkedPayloadFailure::TruncatedHeader);
		const uint32 ChunkCount = Candidate.HeaderWords[Format.ChunkCountWordIndex];
		if (Candidate.HeaderWords[Format.HeaderSizeWordIndex] != ChunkedPayloadHeaderSize
			|| TableOffset != ChunkedPayloadHeaderSize)
			return ChunkedPayloadFail(EChunkedPayloadFailure::InvalidHeader);
		if (ChunkCount < Format.RequiredChunkCount || ChunkCount > Format.MaximumChunkCount)
			return ChunkedPayloadFail(EChunkedPayloadFailure::InvalidChunkCount);
		if (!Reader.IsValid() || StoredSize != Bytes.size() || StoredSize > Format.MaximumBytes
			|| TotalDecodedSize > Format.MaximumBytes)
			return ChunkedPayloadFail(EChunkedPayloadFailure::InvalidStoredSize);
		if (FXxHash64::HashBuffer(Bytes.subspan(ChunkedPayloadHeaderSize)).HashValue != StoredHash)
			return ChunkedPayloadFail(EChunkedPayloadFailure::ChecksumMismatch);

		uint64 TableBytes = 0, TableEnd = 0;
		if (!BulkContainer::TryMultiply(
			ChunkCount, ChunkedPayloadEntrySize, StoredSize, TableBytes)
			|| !BulkContainer::TryAdd(TableOffset, TableBytes, StoredSize, TableEnd))
			return ChunkedPayloadFail(EChunkedPayloadFailure::TableOutOfRange);
		FByteView Table;
		if (!BulkContainer::TryProjectRange(Bytes, TableOffset, TableBytes, Table))
			return ChunkedPayloadFail(EChunkedPayloadFailure::TableOutOfRange);

		BulkContainer::FBoundedReader TableReader(Table, TableBytes);
		std::vector<FChunkRecord> Records(ChunkCount);
		std::vector<BulkContainer::FPayloadRange> Ranges;
		Ranges.reserve(ChunkCount);
		std::unordered_set<uint32> Types;
		std::vector<int32> RequiredIndices(Format.RequiredChunkCount, -1);
		uint64 DecodedSum = 0;
		bool HasCompressedChunk = false;
		for (uint32 Index = 0; Index < ChunkCount; ++Index)
		{
			FChunkRecord& Chunk = Records[Index];
			if (!TableReader.Read(Chunk.Type) || !TableReader.Read(Chunk.Flags)
				|| !TableReader.Read(Chunk.Offset) || !TableReader.Read(Chunk.StoredSize)
				|| !TableReader.Read(Chunk.DecodedSize))
				return ChunkedPayloadFail(EChunkedPayloadFailure::TableOutOfRange);
			if (Chunk.Type == 0 || (Format.RequireUniqueChunkTypes && !Types.insert(Chunk.Type).second))
				return ChunkedPayloadFail(EChunkedPayloadFailure::DuplicateChunkType);
			if ((Chunk.Flags & ~Format.KnownChunkFlags) != 0)
				return ChunkedPayloadFail(EChunkedPayloadFailure::UnsupportedChunkFlags,
					EChunkedPayloadFailureKind::Incompatible);
			const uint32 Compression = Format.CompressionMask == 0 ? 0
				: (Chunk.Flags & Format.CompressionMask) >> Format.CompressionShift;
			if (Compression > Format.MaximumCompressionMethod)
				return ChunkedPayloadFail(EChunkedPayloadFailure::UnsupportedCompression,
					EChunkedPayloadFailureKind::Incompatible);
			if (Chunk.DecodedSize > Format.MaximumBytes - DecodedSum
				|| (Compression == 0 && Chunk.StoredSize != Chunk.DecodedSize))
				return ChunkedPayloadFail(EChunkedPayloadFailure::InvalidChunkSize);
			if (Chunk.Offset % Format.Alignment != 0)
				return ChunkedPayloadFail(EChunkedPayloadFailure::InvalidChunkLayout);
			if (Compression != 0)
			{
				HasCompressedChunk = true;
				if (Chunk.StoredSize == 0 || Format.MaximumCompressionRatio == 0
					|| Chunk.DecodedSize / Chunk.StoredSize > Format.MaximumCompressionRatio
					|| (Chunk.DecodedSize / Chunk.StoredSize == Format.MaximumCompressionRatio
						&& Chunk.DecodedSize % Chunk.StoredSize != 0))
					return ChunkedPayloadFail(EChunkedPayloadFailure::CompressionRatioExceeded);
			}
			DecodedSum += Chunk.DecodedSize;
			Ranges.push_back({Chunk.Offset, Chunk.StoredSize, Format.Alignment});

			if (Chunk.Type <= Format.RequiredChunkCount)
			{
				const uint32 RequiredIndex = Chunk.Type - 1;
				if ((Chunk.Flags & Format.RequiredChunkFlag) == 0
					|| RequiredIndices[RequiredIndex] >= 0
					|| (Format.RequireNonemptyRequiredChunks && Chunk.StoredSize == 0))
					return ChunkedPayloadFail(EChunkedPayloadFailure::MissingRequiredChunk);
				RequiredIndices[RequiredIndex] = static_cast<int32>(Index);
			}
			else if ((Chunk.Flags & Format.RequiredChunkFlag) != 0)
				return ChunkedPayloadFail(EChunkedPayloadFailure::UnknownRequiredChunk,
					EChunkedPayloadFailureKind::Incompatible);
		}

		const BulkContainer::FLayoutPolicy LayoutPolicy{
			.MaximumCount = Format.MaximumChunkCount,
			.MaximumPayloadBytes = Format.MaximumBytes,
			.MaximumContainerBytes = Format.MaximumBytes,
			.RequireCanonicalOffsets = false,
			.AllowTrailingZeroPadding = Format.AllowTrailingZeroPadding};
		BulkContainer::FFailure LayoutFailure;
		if (!BulkContainer::ValidateLayout(
			Bytes, TableEnd, TableEnd, Ranges, LayoutPolicy, &LayoutFailure))
		{
			if (LayoutFailure.Category == BulkContainer::EFailure::NonzeroPadding
				|| LayoutFailure.Category == BulkContainer::EFailure::TrailingNonzeroPadding)
				return ChunkedPayloadFail(EChunkedPayloadFailure::NonzeroPadding);
			if (LayoutFailure.Category == BulkContainer::EFailure::TrailingBytes)
				return ChunkedPayloadFail(EChunkedPayloadFailure::TrailingData);
			return ChunkedPayloadFail(EChunkedPayloadFailure::InvalidChunkLayout);
		}
		if (DecodedSum != TotalDecodedSize)
			return ChunkedPayloadFail(EChunkedPayloadFailure::DecodedSizeMismatch);
		if (std::ranges::any_of(RequiredIndices, [](int32 Index) { return Index < 0; }))
			return ChunkedPayloadFail(EChunkedPayloadFailure::MissingRequiredChunk);
		if (Format.GlobalFlagsWordIndex < 8
			&& HasCompressedChunk != ((Candidate.HeaderWords[Format.GlobalFlagsWordIndex]
				& Format.GlobalCompressedFlag) != 0))
			return ChunkedPayloadFail(EChunkedPayloadFailure::CompressionFlagsMismatch);
		if (HasCompressedChunk)
			return ChunkedPayloadFail(EChunkedPayloadFailure::CompressionUnavailable,
				EChunkedPayloadFailureKind::Incompatible);

		Candidate.Chunks.reserve(Records.size());
		for (const FChunkRecord& Chunk : Records)
		{
			FByteView ChunkBytes;
			if (!BulkContainer::TryProjectRange(Bytes, Chunk.Offset, Chunk.StoredSize, ChunkBytes))
				return ChunkedPayloadFail(EChunkedPayloadFailure::InvalidChunkLayout);
			Candidate.Chunks.push_back({Chunk.Type, Chunk.Flags, ChunkBytes, Chunk.DecodedSize});
		}
		Candidate.RequiredChunks.reserve(RequiredIndices.size());
		for (int32 Index : RequiredIndices)
			Candidate.RequiredChunks.push_back(Candidate.Chunks[static_cast<size_t>(Index)].Bytes);
		OutPayload = std::move(Candidate);
		return {};
	}

	auto DescribeChunkedPayloadFailure(
		EChunkedPayloadFailure Failure,
		std::string_view PayloadName) -> std::string
	{
		const std::string_view Detail = [&]() -> std::string_view {
			switch (Failure)
			{
			case EChunkedPayloadFailure::None: return "is valid.";
			case EChunkedPayloadFailure::InvalidArgument: return "codec arguments are invalid.";
			case EChunkedPayloadFailure::TruncatedHeader: return "header is truncated.";
			case EChunkedPayloadFailure::InvalidHeader: return "header fields are invalid.";
			case EChunkedPayloadFailure::InvalidChunkCount: return "chunk count is invalid.";
			case EChunkedPayloadFailure::InvalidStoredSize: return "stored or decoded size is invalid.";
			case EChunkedPayloadFailure::ChecksumMismatch: return "checksum does not match.";
			case EChunkedPayloadFailure::TableOutOfRange: return "chunk table is out of range.";
			case EChunkedPayloadFailure::DuplicateChunkType: return "chunk types are duplicated.";
			case EChunkedPayloadFailure::UnsupportedChunkFlags: return "chunk flags are unsupported.";
			case EChunkedPayloadFailure::UnsupportedCompression: return "chunk compression method is unsupported.";
			case EChunkedPayloadFailure::InvalidChunkLayout: return "chunks are misaligned, overlapping, or out of range.";
			case EChunkedPayloadFailure::InvalidChunkSize: return "chunk decoded size is invalid.";
			case EChunkedPayloadFailure::CompressionRatioExceeded: return "chunk exceeds the maximum compression ratio.";
			case EChunkedPayloadFailure::NonzeroPadding: return "alignment padding is nonzero.";
			case EChunkedPayloadFailure::UnknownRequiredChunk: return "contains an unknown required chunk.";
			case EChunkedPayloadFailure::MissingRequiredChunk: return "required chunks are missing or invalid.";
			case EChunkedPayloadFailure::DecodedSizeMismatch: return "decoded size does not match its chunks.";
			case EChunkedPayloadFailure::CompressionFlagsMismatch: return "compression flags are inconsistent.";
			case EChunkedPayloadFailure::TrailingData: return "contains trailing data.";
			case EChunkedPayloadFailure::CompressionUnavailable: return "uses compression unavailable in this build.";
			}
			return "is invalid.";
		}();
		return std::format("{} {}", PayloadName, Detail);
	}
}
