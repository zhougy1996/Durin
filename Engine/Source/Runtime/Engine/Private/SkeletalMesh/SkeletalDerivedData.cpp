#include "SkeletalMesh/SkeletalDerivedData.h"

#include "PayloadDecodeResult.h"
#include "Serialization/EngineWire.h"
#include "Serialization/Archive.h"

namespace Durin
{
	thread_local std::vector<FScopedSkeletalDerivedDataRepairLoad*> GSkeletalDerivedDataRepairLoads;

	FScopedSkeletalDerivedDataRepairLoad::FScopedSkeletalDerivedDataRepairLoad()
	{
		GSkeletalDerivedDataRepairLoads.push_back(this);
	}

	FScopedSkeletalDerivedDataRepairLoad::~FScopedSkeletalDerivedDataRepairLoad()
	{
		check(!GSkeletalDerivedDataRepairLoads.empty()
			&& GSkeletalDerivedDataRepairLoads.back() == this);
		GSkeletalDerivedDataRepairLoads.pop_back();
	}

	auto IsSkeletalDerivedDataRepairLoadActive() -> bool
	{
		return !GSkeletalDerivedDataRepairLoads.empty();
	}

	auto ReportMissingSkeletalDerivedDataAsset(DObject* Asset) -> void
	{
		if (!Asset) return;
		for (FScopedSkeletalDerivedDataRepairLoad* Scope : GSkeletalDerivedDataRepairLoads)
			if (std::ranges::find(Scope->MissingAssets, Asset) == Scope->MissingAssets.end())
				Scope->MissingAssets.push_back(Asset);
	}

	namespace
	{
		inline constexpr uint32 ChunkRequired = 1;
		enum class EMeshChunk : uint32
		{
			Metadata = 1,
			Sections = 2,
			Positions = 3,
			VertexAttributes = 4,
			Indices = 5,
			Influences = 6,
			PaletteAndInverseBinds = 7
		};

		enum class EAnimationChunk : uint32
		{
			Metadata = 1,
			Tracks = 2,
			Times = 3,
			Values = 4
		};

		using FWriter = EngineWire::FWriter;
		using FReader = EngineWire::FReader;

		struct FChunkBytes
		{
			uint32 Type = 0;
			std::vector<uint8> Bytes;
		};

		struct FChunkRecord
		{
			uint32 Type = 0;
			uint32 Flags = 0;
			uint64 Offset = 0;
			uint64 StoredSize = 0;
			uint64 DecodedSize = 0;
		};

		auto Align16(uint64 Value, uint64& OutValue) -> bool
		{
			if (Value > std::numeric_limits<uint64>::max() - (SkeletalPayloadAlignment - 1))
				return false;
			OutValue = (Value + SkeletalPayloadAlignment - 1)
				& ~(static_cast<uint64>(SkeletalPayloadAlignment) - 1);
			return true;
		}

		using EngineWire::ReadLittleEndianAt;

		auto WriteVector(FWriter& Writer, const FVector2f& Value) -> void
		{
			Writer.WriteFloat(Value.x); Writer.WriteFloat(Value.y);
		}

		auto WriteVector(FWriter& Writer, const FVector3f& Value) -> void
		{
			Writer.WriteFloat(Value.x); Writer.WriteFloat(Value.y); Writer.WriteFloat(Value.z);
		}

		auto WriteVector(FWriter& Writer, const FVector4f& Value) -> void
		{
			Writer.WriteFloat(Value.x); Writer.WriteFloat(Value.y);
			Writer.WriteFloat(Value.z); Writer.WriteFloat(Value.w);
		}

		auto ReadVector(FReader& Reader, FVector2f& Value) -> bool
		{
			return Reader.ReadFloat(Value.x) && Reader.ReadFloat(Value.y);
		}

		auto ReadVector(FReader& Reader, FVector3f& Value) -> bool
		{
			return Reader.ReadFloat(Value.x) && Reader.ReadFloat(Value.y)
				&& Reader.ReadFloat(Value.z);
		}

		auto ReadVector(FReader& Reader, FVector4f& Value) -> bool
		{
			return Reader.ReadFloat(Value.x) && Reader.ReadFloat(Value.y)
				&& Reader.ReadFloat(Value.z) && Reader.ReadFloat(Value.w);
		}

		auto WriteBox(FWriter& Writer, const FBox& Bounds) -> void
		{
			Writer.WriteU32(Bounds.bIsValid ? 1u : 0u);
			for (const double Value : {Bounds.Min.x, Bounds.Min.y, Bounds.Min.z,
				Bounds.Max.x, Bounds.Max.y, Bounds.Max.z})
				Writer.WriteFloat(static_cast<float>(Value));
		}

		auto ReadBox(FReader& Reader, FBox& Bounds) -> bool
		{
			uint32 Valid = 0;
			std::array<float, 6> Values{};
			if (!Reader.ReadU32(Valid) || Valid != 1) return false;
			for (float& Value : Values)
				if (!Reader.ReadFloat(Value)) return false;
			Bounds = FBox(
				FVector3(Values[0], Values[1], Values[2]),
				FVector3(Values[3], Values[4], Values[5]));
			return true;
		}

		auto BuildContainer(
			uint32 Magic,
			uint32 SchemaVersion,
			uint32 ProducerVersion,
			ESkeletalPayloadTargetPlatform TargetPlatform,
			ESkeletalPayloadTargetProfile TargetProfile,
			std::span<const FChunkBytes> ChunkBytes,
			uint64 MaximumBytes,
			std::vector<uint8>& OutBytes,
			std::string& OutError) -> bool
		{
			if (TargetPlatform != ESkeletalPayloadTargetPlatform::Win64
				|| TargetProfile != ESkeletalPayloadTargetProfile::Game)
				return Fail("A concrete skeletal payload target and profile are required.", &OutError);
			if (ChunkBytes.empty() || ChunkBytes.size() > MaximumSkeletalPayloadChunks)
				return Fail("Skeletal payload chunk count is invalid.", &OutError);

			uint64 Offset = SkeletalPayloadHeaderSize
				+ static_cast<uint64>(ChunkBytes.size()) * SkeletalPayloadChunkEntrySize;
			uint64 TotalDecoded = 0;
			std::vector<FChunkRecord> Records;
			Records.reserve(ChunkBytes.size());
			for (const FChunkBytes& Chunk : ChunkBytes)
			{
				if (!Align16(Offset, Offset) || Offset > MaximumBytes
					|| Chunk.Bytes.size() > MaximumBytes - Offset
					|| TotalDecoded > MaximumBytes - Chunk.Bytes.size())
					return Fail("Skeletal payload size overflowed its bound.", &OutError);
				Records.push_back({
					.Type = Chunk.Type,
					.Flags = ChunkRequired,
					.Offset = Offset,
					.StoredSize = Chunk.Bytes.size(),
					.DecodedSize = Chunk.Bytes.size()});
				Offset += Chunk.Bytes.size();
				TotalDecoded += Chunk.Bytes.size();
			}
			if (Offset > MaximumBytes)
				return Fail("Skeletal payload exceeds its byte limit.", &OutError);

			FWriter Body;
			for (const FChunkRecord& Record : Records)
			{
				Body.WriteU32(Record.Type);
				Body.WriteU32(Record.Flags);
				Body.WriteU64(Record.Offset);
				Body.WriteU64(Record.StoredSize);
				Body.WriteU64(Record.DecodedSize);
			}
			uint64 BodyOffset = SkeletalPayloadHeaderSize + Body.GetBytes().size();
			for (size_t Index = 0; Index < Records.size(); ++Index)
			{
				Body.WriteZeroes(static_cast<size_t>(Records[Index].Offset - BodyOffset));
				Body.WriteBytes(ChunkBytes[Index].Bytes);
				BodyOffset = Records[Index].Offset + Records[Index].StoredSize;
			}
			const std::vector<uint8> StoredBody = Body.TakeBytes();

			FWriter Result;
			Result.WriteU32(Magic);
			Result.WriteU32(SchemaVersion);
			Result.WriteU32(ProducerVersion);
			Result.WriteU32(static_cast<uint32>(TargetPlatform));
			Result.WriteU32(static_cast<uint32>(TargetProfile));
			Result.WriteU32(0);
			Result.WriteU32(SkeletalPayloadHeaderSize);
			Result.WriteU32(static_cast<uint32>(Records.size()));
			Result.WriteU64(SkeletalPayloadHeaderSize);
			Result.WriteU64(TotalDecoded);
			Result.WriteU64(Offset);
			Result.WriteU64(FXxHash64::HashBuffer(StoredBody).HashValue);
			Result.WriteBytes(StoredBody);
			OutBytes = Result.TakeBytes();
			OutError.clear();
			return true;
		}

		auto ParseContainer(
			std::span<const uint8> Bytes,
			uint32 ExpectedMagic,
			uint32 ExpectedSchema,
			ESkeletalPayloadTargetPlatform ExpectedPlatform,
			ESkeletalPayloadTargetProfile ExpectedProfile,
			uint32 RequiredChunkCount,
			uint64 MaximumBytes,
			std::vector<std::span<const uint8>>& OutRequiredChunks,
			std::string& OutError,
			EPayloadDecodeError& OutCode) -> bool
		{
			OutCode = EPayloadDecodeError::Corrupt;
			if (Bytes.size() < SkeletalPayloadHeaderSize)
				return Fail("Skeletal payload header is truncated.", &OutError);
			if (ExpectedPlatform != ESkeletalPayloadTargetPlatform::Win64
				|| ExpectedProfile != ESkeletalPayloadTargetProfile::Game)
			{
				OutCode = EPayloadDecodeError::Incompatible;
				return Fail("A concrete skeletal payload target and profile are required.", &OutError);
			}

			uint32 Magic = 0, Schema = 0, Producer = 0, Platform = 0, Profile = 0;
			uint32 Flags = 0, HeaderSize = 0, ChunkCount = 0;
			uint64 TableOffset = 0, TotalDecoded = 0, StoredSize = 0, BodyHash = 0;
			if (!ReadLittleEndianAt(Bytes, 0, Magic) || !ReadLittleEndianAt(Bytes, 4, Schema)
				|| !ReadLittleEndianAt(Bytes, 8, Producer) || !ReadLittleEndianAt(Bytes, 12, Platform)
				|| !ReadLittleEndianAt(Bytes, 16, Profile) || !ReadLittleEndianAt(Bytes, 20, Flags)
				|| !ReadLittleEndianAt(Bytes, 24, HeaderSize) || !ReadLittleEndianAt(Bytes, 28, ChunkCount)
				|| !ReadLittleEndianAt(Bytes, 32, TableOffset) || !ReadLittleEndianAt(Bytes, 40, TotalDecoded)
				|| !ReadLittleEndianAt(Bytes, 48, StoredSize) || !ReadLittleEndianAt(Bytes, 56, BodyHash))
				return Fail("Skeletal payload header is truncated.", &OutError);
			if (Magic != ExpectedMagic) return Fail("Skeletal payload magic is invalid.", &OutError);
			if (Schema != ExpectedSchema)
			{
				OutCode = EPayloadDecodeError::Incompatible;
				return Fail("Skeletal payload schema is unsupported.", &OutError);
			}
			if (Producer == 0)
				return Fail("Skeletal payload producer metadata is invalid.", &OutError);
			if (Platform != static_cast<uint32>(ExpectedPlatform)
				|| Profile != static_cast<uint32>(ExpectedProfile))
			{
				OutCode = EPayloadDecodeError::Incompatible;
				return Fail("Skeletal payload target or profile does not match.", &OutError);
			}
			if (Flags != 0 || HeaderSize != SkeletalPayloadHeaderSize
				|| TableOffset != SkeletalPayloadHeaderSize
				|| ChunkCount < RequiredChunkCount || ChunkCount > MaximumSkeletalPayloadChunks)
				return Fail("Skeletal payload header fields are invalid.", &OutError);
			if (StoredSize != Bytes.size() || StoredSize > MaximumBytes
				|| TotalDecoded > MaximumBytes)
				return Fail("Skeletal payload stored or decoded size is invalid.", &OutError);
			if (FXxHash64::HashBuffer(Bytes.subspan(SkeletalPayloadHeaderSize)).HashValue != BodyHash)
				return Fail("Skeletal payload checksum does not match.", &OutError);

			const uint64 TableSize = static_cast<uint64>(ChunkCount) * SkeletalPayloadChunkEntrySize;
			const uint64 TableEnd = TableOffset + TableSize;
			if (TableEnd < TableOffset || TableEnd > StoredSize)
				return Fail("Skeletal payload chunk table is out of range.", &OutError);
			OutRequiredChunks.assign(RequiredChunkCount, {});
			std::unordered_set<uint32> Types;
			uint64 PreviousEnd = TableEnd;
			uint64 DecodedSum = 0;
			for (uint32 Index = 0; Index < ChunkCount; ++Index)
			{
				const size_t Entry = static_cast<size_t>(TableOffset
					+ static_cast<uint64>(Index) * SkeletalPayloadChunkEntrySize);
				FChunkRecord Chunk;
				if (!ReadLittleEndianAt(Bytes, Entry, Chunk.Type)
					|| !ReadLittleEndianAt(Bytes, Entry + 4, Chunk.Flags)
					|| !ReadLittleEndianAt(Bytes, Entry + 8, Chunk.Offset)
					|| !ReadLittleEndianAt(Bytes, Entry + 16, Chunk.StoredSize)
					|| !ReadLittleEndianAt(Bytes, Entry + 24, Chunk.DecodedSize))
					return Fail("Skeletal payload chunk table is truncated.", &OutError);
				if (!Types.insert(Chunk.Type).second)
					return Fail("Skeletal payload chunk types are duplicated.", &OutError);
				if ((Chunk.Flags & ~ChunkRequired) != 0)
				{
					OutCode = EPayloadDecodeError::Incompatible;
					return Fail("Skeletal payload chunk flags are unsupported.", &OutError);
				}
				if (Chunk.Offset % SkeletalPayloadAlignment != 0 || Chunk.Offset < PreviousEnd
					|| Chunk.Offset > StoredSize || Chunk.StoredSize > StoredSize - Chunk.Offset)
					return Fail("Skeletal payload chunks are misaligned, overlapping, or out of range.", &OutError);
				if (Chunk.DecodedSize != Chunk.StoredSize
					|| DecodedSum > MaximumBytes - Chunk.DecodedSize)
					return Fail("Skeletal payload chunk decoded size is invalid.", &OutError);
				for (uint64 Padding = PreviousEnd; Padding < Chunk.Offset; ++Padding)
					if (Bytes[static_cast<size_t>(Padding)] != 0)
						return Fail("Skeletal payload alignment padding is nonzero.", &OutError);
				PreviousEnd = Chunk.Offset + Chunk.StoredSize;
				DecodedSum += Chunk.DecodedSize;
				if (Chunk.Type >= 1 && Chunk.Type <= RequiredChunkCount)
				{
					if ((Chunk.Flags & ChunkRequired) == 0)
						return Fail("Skeletal payload required chunk is not marked required.", &OutError);
					OutRequiredChunks[Chunk.Type - 1] = Bytes.subspan(
						static_cast<size_t>(Chunk.Offset), static_cast<size_t>(Chunk.StoredSize));
				}
				else if ((Chunk.Flags & ChunkRequired) != 0)
				{
					OutCode = EPayloadDecodeError::Incompatible;
					return Fail("Skeletal payload contains an unknown required chunk.", &OutError);
				}
			}
			if (PreviousEnd != StoredSize)
				return Fail("Skeletal payload contains trailing data.", &OutError);
			if (DecodedSum != TotalDecoded
				|| std::ranges::any_of(OutRequiredChunks, [](std::span<const uint8> Chunk) {
					return Chunk.empty();
				}))
				return Fail("Skeletal payload required chunks or decoded size are invalid.", &OutError);
			OutError.clear();
			return true;
		}

	}

	auto BuildSkeletalMeshSerializedValue(
		const FSkeletalMeshPayloadData& Payload,
		const FSkeletalPayloadSerializationContext& Context,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool
	{
		if (!ValidateSkeletalMeshPayload(
			Payload, Context.SkeletonBoneCount, Context.MaterialSlotCount, OutError))
			return false;
		std::vector<FChunkBytes> Chunks;
		Chunks.reserve(7);

		FWriter Metadata;
		Metadata.WriteU32(Context.MaterialSlotCount);
		Metadata.WriteU32(static_cast<uint32>(Payload.Positions.size()));
		Metadata.WriteU32(static_cast<uint32>(Payload.Normals.size()));
		Metadata.WriteU32(static_cast<uint32>(Payload.Tangents.size()));
		Metadata.WriteU32(static_cast<uint32>(Payload.Colors.size()));
		Metadata.WriteU32(static_cast<uint32>(Payload.Indices.size()));
		Metadata.WriteU32(static_cast<uint32>(Payload.Influences.size()));
		Metadata.WriteU32(static_cast<uint32>(Payload.Sections.size()));
		Metadata.WriteU32(static_cast<uint32>(Payload.PaletteBoneIndices.size()));
		Metadata.WriteU32(static_cast<uint32>(Payload.InverseBindMatrices.size()));
		Metadata.WriteU32(MaximumSkeletalMeshUVChannels);
		for (const auto& UVs : Payload.UVChannels)
			Metadata.WriteU32(static_cast<uint32>(UVs.size()));
		WriteBox(Metadata, Payload.LocalBounds);
		Chunks.push_back({static_cast<uint32>(EMeshChunk::Metadata), Metadata.TakeBytes()});

		FWriter Sections;
		Sections.WriteU32(static_cast<uint32>(Payload.Sections.size()));
		for (const FSkeletalMeshSection& Section : Payload.Sections)
		{
			const std::string Name = Section.Name.ToString();
			if (Name.empty() || Name.size() > MaximumSkeletalPayloadNameBytes)
				return Fail("Skeletal-mesh section name is outside its wire bound.", &OutError);
			Sections.WriteString(Name);
			Sections.WriteU32(Section.FirstIndex);
			Sections.WriteU32(Section.IndexCount);
			Sections.WriteU32(Section.MinVertexIndex);
			Sections.WriteU32(Section.MaxVertexIndex);
			Sections.WriteU32(Section.MaterialSlotIndex);
			WriteBox(Sections, Section.LocalBounds);
		}
		Chunks.push_back({static_cast<uint32>(EMeshChunk::Sections), Sections.TakeBytes()});

		FWriter Positions;
		Positions.WriteU32(static_cast<uint32>(Payload.Positions.size()));
		for (const FVector3f& Value : Payload.Positions) WriteVector(Positions, Value);
		Chunks.push_back({static_cast<uint32>(EMeshChunk::Positions), Positions.TakeBytes()});

		FWriter Attributes;
		Attributes.WriteU32(static_cast<uint32>(Payload.Normals.size()));
		for (const FVector3f& Value : Payload.Normals) WriteVector(Attributes, Value);
		Attributes.WriteU32(static_cast<uint32>(Payload.Tangents.size()));
		for (const FVector4f& Value : Payload.Tangents) WriteVector(Attributes, Value);
		Attributes.WriteU32(MaximumSkeletalMeshUVChannels);
		for (const auto& UVs : Payload.UVChannels)
		{
			Attributes.WriteU32(static_cast<uint32>(UVs.size()));
			for (const FVector2f& Value : UVs) WriteVector(Attributes, Value);
		}
		Attributes.WriteU32(static_cast<uint32>(Payload.Colors.size()));
		for (const FVector4f& Value : Payload.Colors) WriteVector(Attributes, Value);
		Chunks.push_back({static_cast<uint32>(EMeshChunk::VertexAttributes), Attributes.TakeBytes()});

		FWriter Indices;
		Indices.WriteU32(static_cast<uint32>(Payload.Indices.size()));
		for (uint32 Index : Payload.Indices) Indices.WriteU32(Index);
		Chunks.push_back({static_cast<uint32>(EMeshChunk::Indices), Indices.TakeBytes()});

		FWriter Influences;
		Influences.WriteU32(static_cast<uint32>(Payload.Influences.size()));
		for (const FSkeletalMeshVertexInfluences& Influence : Payload.Influences)
		{
			Influences.WriteU8(Influence.Count);
			Influences.WriteU8(0);
			Influences.WriteU16(0);
			for (uint32 Slot = 0; Slot < MaximumSkeletalMeshInfluences; ++Slot)
			{
				Influences.WriteU16(Influence.BoneIndices[Slot]);
				Influences.WriteU16(0);
				Influences.WriteFloat(Influence.Weights[Slot]);
			}
		}
		Chunks.push_back({static_cast<uint32>(EMeshChunk::Influences), Influences.TakeBytes()});

		FWriter Palette;
		Palette.WriteU32(static_cast<uint32>(Payload.PaletteBoneIndices.size()));
		for (uint16 Bone : Payload.PaletteBoneIndices) Palette.WriteU16(Bone);
		Palette.WriteU32(static_cast<uint32>(Payload.InverseBindMatrices.size()));
		for (const FMatrix4f& Matrix : Payload.InverseBindMatrices)
			for (uint32 Row = 0; Row < 4; ++Row)
				for (uint32 Column = 0; Column < 4; ++Column)
					Palette.WriteFloat(Matrix[Row][Column]);
		Chunks.push_back({static_cast<uint32>(EMeshChunk::PaletteAndInverseBinds), Palette.TakeBytes()});

		return BuildContainer(
			SkeletalMeshPayloadMagic, SkeletalMeshPayloadSchemaVersion,
			SkeletalMeshPayloadProducerVersion, Context.TargetPlatform,
			Context.TargetProfile, Chunks,
			MaximumSkeletalMeshPayloadBytes, OutBytes, OutError);
	}

	auto ParseSkeletalMeshSerializedValue(
		std::span<const uint8> Bytes,
		const FSkeletalPayloadSerializationContext& Context,
		FSkeletalMeshPayloadData& OutPayload) -> FPayloadDecodeResult
	{
		std::vector<std::span<const uint8>> Chunks;
		std::string Error;
		EPayloadDecodeError Code = EPayloadDecodeError::Corrupt;
		if (!ParseContainer(Bytes, SkeletalMeshPayloadMagic,
			SkeletalMeshPayloadSchemaVersion,
			Context.TargetPlatform, Context.TargetProfile, 7,
			MaximumSkeletalMeshPayloadBytes,
			Chunks, Error, Code)) return {Code, std::move(Error)};

		FSkeletalMeshPayloadData Candidate;
		uint32 EncodedMaterialSlots = 0, PositionCount = 0, NormalCount = 0;
		uint32 TangentCount = 0, ColorCount = 0, IndexCount = 0;
		uint32 InfluenceCount = 0, SectionCount = 0, PaletteCount = 0;
		uint32 InverseBindCount = 0, UVChannelCount = 0;
		std::array<uint32, MaximumSkeletalMeshUVChannels> UVCounts{};
		FReader Metadata(Chunks[0]);
		if (!Metadata.ReadU32(EncodedMaterialSlots) || !Metadata.ReadU32(PositionCount)
			|| !Metadata.ReadU32(NormalCount) || !Metadata.ReadU32(TangentCount)
			|| !Metadata.ReadU32(ColorCount) || !Metadata.ReadU32(IndexCount)
			|| !Metadata.ReadU32(InfluenceCount) || !Metadata.ReadU32(SectionCount)
			|| !Metadata.ReadU32(PaletteCount) || !Metadata.ReadU32(InverseBindCount)
			|| !Metadata.ReadU32(UVChannelCount))
			return {EPayloadDecodeError::Corrupt, "Skeletal-mesh metadata chunk is truncated."};
		for (uint32& Count : UVCounts)
			if (!Metadata.ReadU32(Count))
				return {EPayloadDecodeError::Corrupt, "Skeletal-mesh metadata UV counts are truncated."};
		if (!ReadBox(Metadata, Candidate.LocalBounds) || !Metadata.AtEnd()
			|| EncodedMaterialSlots != Context.MaterialSlotCount
			|| PositionCount == 0 || PositionCount > MaximumSkeletalMeshVertices
			|| NormalCount != PositionCount || TangentCount != PositionCount
			|| InfluenceCount != PositionCount
			|| ColorCount != 0 && ColorCount != PositionCount
			|| IndexCount == 0 || IndexCount > MaximumSkeletalMeshIndices
			|| SectionCount == 0 || SectionCount > MaximumSkeletalMeshSections
			|| PaletteCount == 0 || PaletteCount != InverseBindCount
			|| PaletteCount > Context.SkeletonBoneCount
			|| UVChannelCount != MaximumSkeletalMeshUVChannels
			|| std::ranges::any_of(UVCounts, [PositionCount](uint32 Count) {
				return Count != 0 && Count != PositionCount;
			}))
			return {EPayloadDecodeError::Corrupt, "Skeletal-mesh metadata counts or bounds are invalid."};

		FReader Sections(Chunks[1]);
		uint32 StoredSectionCount = 0;
		if (!Sections.ReadU32(StoredSectionCount) || StoredSectionCount != SectionCount)
			return {EPayloadDecodeError::Corrupt, "Skeletal-mesh section count does not match metadata."};
		Candidate.Sections.resize(SectionCount);
		for (FSkeletalMeshSection& Section : Candidate.Sections)
		{
			std::string Name;
			if (!Sections.ReadString(Name, MaximumSkeletalPayloadNameBytes) || !Sections.ReadU32(Section.FirstIndex)
				|| !Sections.ReadU32(Section.IndexCount)
				|| !Sections.ReadU32(Section.MinVertexIndex)
				|| !Sections.ReadU32(Section.MaxVertexIndex)
				|| !Sections.ReadU32(Section.MaterialSlotIndex)
				|| !ReadBox(Sections, Section.LocalBounds))
				return {EPayloadDecodeError::Corrupt, "Skeletal-mesh section record is truncated."};
			Section.Name = FName(Name);
		}
		if (!Sections.AtEnd())
			return {EPayloadDecodeError::Corrupt, "Skeletal-mesh section chunk has trailing data."};

		FReader Positions(Chunks[2]);
		uint32 StoredPositionCount = 0;
		if (!Positions.ReadU32(StoredPositionCount) || StoredPositionCount != PositionCount
			|| Positions.Remaining() != static_cast<uint64>(PositionCount) * sizeof(float) * 3)
			return {EPayloadDecodeError::Corrupt, "Skeletal-mesh position chunk size is invalid."};
		Candidate.Positions.resize(PositionCount);
		for (FVector3f& Value : Candidate.Positions)
			if (!ReadVector(Positions, Value))
				return {EPayloadDecodeError::Corrupt, "Skeletal-mesh position chunk is truncated."};

		FReader Attributes(Chunks[3]);
		auto ReadVector3Array = [&](std::vector<FVector3f>& Values, uint32 Expected) -> bool {
			uint32 Count = 0;
			if (!Attributes.ReadU32(Count) || Count != Expected
				|| Attributes.Remaining() < static_cast<uint64>(Count) * 3 * sizeof(float))
				return false;
			Values.resize(Count);
			return std::ranges::all_of(Values, [&](FVector3f& Value) {
				return ReadVector(Attributes, Value);
			});
		};
		auto ReadVector4Array = [&](std::vector<FVector4f>& Values, uint32 Expected) -> bool {
			uint32 Count = 0;
			if (!Attributes.ReadU32(Count) || Count != Expected
				|| Attributes.Remaining() < static_cast<uint64>(Count) * 4 * sizeof(float))
				return false;
			Values.resize(Count);
			return std::ranges::all_of(Values, [&](FVector4f& Value) {
				return ReadVector(Attributes, Value);
			});
		};
		if (!ReadVector3Array(Candidate.Normals, NormalCount)
			|| !ReadVector4Array(Candidate.Tangents, TangentCount))
			return {EPayloadDecodeError::Corrupt, "Skeletal-mesh vertex attribute chunk is truncated."};
		uint32 StoredUVChannels = 0;
		if (!Attributes.ReadU32(StoredUVChannels)
			|| StoredUVChannels != MaximumSkeletalMeshUVChannels)
			return {EPayloadDecodeError::Corrupt, "Skeletal-mesh UV channel count is invalid."};
		for (uint32 Channel = 0; Channel < MaximumSkeletalMeshUVChannels; ++Channel)
		{
			uint32 Count = 0;
			if (!Attributes.ReadU32(Count) || Count != UVCounts[Channel]
				|| Attributes.Remaining() < static_cast<uint64>(Count) * 2 * sizeof(float))
				return {EPayloadDecodeError::Corrupt, "Skeletal-mesh UV count does not match metadata."};
			Candidate.UVChannels[Channel].resize(Count);
			for (FVector2f& Value : Candidate.UVChannels[Channel])
				if (!ReadVector(Attributes, Value))
					return {EPayloadDecodeError::Corrupt, "Skeletal-mesh UV data is truncated."};
		}
		if (!ReadVector4Array(Candidate.Colors, ColorCount) || !Attributes.AtEnd())
			return {EPayloadDecodeError::Corrupt, "Skeletal-mesh color data or trailing attributes are invalid."};

		FReader Indices(Chunks[4]);
		uint32 StoredIndexCount = 0;
		if (!Indices.ReadU32(StoredIndexCount) || StoredIndexCount != IndexCount
			|| Indices.Remaining() != static_cast<uint64>(IndexCount) * sizeof(uint32))
			return {EPayloadDecodeError::Corrupt, "Skeletal-mesh index chunk size is invalid."};
		Candidate.Indices.resize(IndexCount);
		for (uint32& Index : Candidate.Indices)
			if (!Indices.ReadU32(Index))
				return {EPayloadDecodeError::Corrupt, "Skeletal-mesh index data is truncated."};

		FReader Influences(Chunks[5]);
		uint32 StoredInfluenceCount = 0;
		constexpr uint64 InfluenceWireBytes = 4 + MaximumSkeletalMeshInfluences * 8;
		if (!Influences.ReadU32(StoredInfluenceCount) || StoredInfluenceCount != InfluenceCount
			|| Influences.Remaining() != static_cast<uint64>(InfluenceCount) * InfluenceWireBytes)
			return {EPayloadDecodeError::Corrupt, "Skeletal-mesh influence chunk size is invalid."};
		Candidate.Influences.resize(InfluenceCount);
		for (FSkeletalMeshVertexInfluences& Influence : Candidate.Influences)
		{
			uint8 Reserved8 = 0;
			uint16 Reserved16 = 0;
			if (!Influences.ReadU8(Influence.Count) || !Influences.ReadU8(Reserved8)
				|| !Influences.ReadU16(Reserved16) || Reserved8 != 0 || Reserved16 != 0)
				return {EPayloadDecodeError::Corrupt, "Skeletal-mesh influence header is invalid."};
			for (uint32 Slot = 0; Slot < MaximumSkeletalMeshInfluences; ++Slot)
			{
				if (!Influences.ReadU16(Influence.BoneIndices[Slot])
					|| !Influences.ReadU16(Reserved16) || Reserved16 != 0
					|| !Influences.ReadFloat(Influence.Weights[Slot]))
					return {EPayloadDecodeError::Corrupt, "Skeletal-mesh influence record is truncated."};
			}
		}

		FReader Palette(Chunks[6]);
		uint32 StoredPaletteCount = 0;
		if (!Palette.ReadU32(StoredPaletteCount) || StoredPaletteCount != PaletteCount
			|| Palette.Remaining() < static_cast<uint64>(PaletteCount) * sizeof(uint16) + 4)
			return {EPayloadDecodeError::Corrupt, "Skeletal-mesh palette count does not match metadata."};
		Candidate.PaletteBoneIndices.resize(PaletteCount);
		for (uint16& Bone : Candidate.PaletteBoneIndices)
			if (!Palette.ReadU16(Bone))
				return {EPayloadDecodeError::Corrupt, "Skeletal-mesh palette is truncated."};
		uint32 StoredInverseBindCount = 0;
		if (!Palette.ReadU32(StoredInverseBindCount)
			|| StoredInverseBindCount != InverseBindCount
			|| Palette.Remaining() != static_cast<uint64>(InverseBindCount) * 16 * sizeof(float))
			return {EPayloadDecodeError::Corrupt, "Skeletal-mesh inverse-bind chunk size is invalid."};
		Candidate.InverseBindMatrices.resize(InverseBindCount);
		for (FMatrix4f& Matrix : Candidate.InverseBindMatrices)
			for (uint32 Row = 0; Row < 4; ++Row)
				for (uint32 Column = 0; Column < 4; ++Column)
					if (!Palette.ReadFloat(Matrix[Row][Column]))
						return {EPayloadDecodeError::Corrupt, "Skeletal-mesh inverse-bind data is truncated."};

		if (!ValidateSkeletalMeshPayload(
			Candidate, Context.SkeletonBoneCount, Context.MaterialSlotCount, Error))
			return {EPayloadDecodeError::Corrupt, std::move(Error)};
		OutPayload = std::move(Candidate);
		return {};
	}

	auto BuildAnimationClipSerializedValue(
		const FAnimationClipPayloadData& Payload,
		const FSkeletalPayloadSerializationContext& Context,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool
	{
		if (!ValidateAnimationClipPayload(
			Payload, Context.SkeletonBoneCount, OutError)) return false;
		uint64 KeyCount = 0;
		for (const FAnimationTrackData& Track : Payload.Tracks) KeyCount += Track.Times.size();
		std::vector<FChunkBytes> Chunks;
		Chunks.reserve(4);

		FWriter Metadata;
		Metadata.WriteFloat(Payload.DurationSeconds);
		Metadata.WriteU32(static_cast<uint32>(Payload.Tracks.size()));
		Metadata.WriteU64(KeyCount);
		Chunks.push_back({static_cast<uint32>(EAnimationChunk::Metadata), Metadata.TakeBytes()});

		FWriter Tracks;
		Tracks.WriteU32(static_cast<uint32>(Payload.Tracks.size()));
		uint64 FirstKey = 0;
		for (const FAnimationTrackData& Track : Payload.Tracks)
		{
			Tracks.WriteU16(Track.BoneIndex);
			Tracks.WriteU8(static_cast<uint8>(Track.Path));
			Tracks.WriteU8(static_cast<uint8>(Track.Interpolation));
			Tracks.WriteU64(FirstKey);
			Tracks.WriteU32(static_cast<uint32>(Track.Times.size()));
			Tracks.WriteU32(0);
			FirstKey += Track.Times.size();
		}
		Chunks.push_back({static_cast<uint32>(EAnimationChunk::Tracks), Tracks.TakeBytes()});

		FWriter Times;
		Times.WriteU64(KeyCount);
		for (const FAnimationTrackData& Track : Payload.Tracks)
			for (float Time : Track.Times) Times.WriteFloat(Time);
		Chunks.push_back({static_cast<uint32>(EAnimationChunk::Times), Times.TakeBytes()});

		FWriter Values;
		Values.WriteU64(KeyCount);
		for (const FAnimationTrackData& Track : Payload.Tracks)
		{
			if (Track.Path == EAnimationTrackPath::Rotation)
				for (const FVector4f& Value : Track.RotationValues) WriteVector(Values, Value);
			else
				for (const FVector3f& Value : Track.VectorValues)
				{
					WriteVector(Values, Value);
					Values.WriteFloat(0.0f);
				}
		}
		Chunks.push_back({static_cast<uint32>(EAnimationChunk::Values), Values.TakeBytes()});

		return BuildContainer(
			AnimationClipPayloadMagic, AnimationClipPayloadSchemaVersion,
			AnimationClipPayloadProducerVersion, Context.TargetPlatform,
			Context.TargetProfile, Chunks,
			MaximumAnimationClipPayloadBytes, OutBytes, OutError);
	}

	auto ParseAnimationClipSerializedValue(
		std::span<const uint8> Bytes,
		const FSkeletalPayloadSerializationContext& Context,
		FAnimationClipPayloadData& OutPayload) -> FPayloadDecodeResult
	{
		std::vector<std::span<const uint8>> Chunks;
		std::string Error;
		EPayloadDecodeError Code = EPayloadDecodeError::Corrupt;
		if (!ParseContainer(Bytes, AnimationClipPayloadMagic,
			AnimationClipPayloadSchemaVersion,
			Context.TargetPlatform, Context.TargetProfile, 4,
			MaximumAnimationClipPayloadBytes,
			Chunks, Error, Code)) return {Code, std::move(Error)};

		FAnimationClipPayloadData Candidate;
		uint32 TrackCount = 0;
		uint64 KeyCount = 0;
		FReader Metadata(Chunks[0]);
		if (!Metadata.ReadFloat(Candidate.DurationSeconds)
			|| !Metadata.ReadU32(TrackCount) || !Metadata.ReadU64(KeyCount)
			|| !Metadata.AtEnd() || TrackCount == 0 || TrackCount > MaximumAnimationClipTracks
			|| KeyCount == 0 || KeyCount > MaximumAnimationKeysPerClip)
			return {EPayloadDecodeError::Corrupt, "Animation payload metadata is invalid."};

		struct FTrackRecord
		{
			uint16 BoneIndex = 0;
			uint8 Path = 0;
			uint8 Interpolation = 0;
			uint64 FirstKey = 0;
			uint32 KeyCount = 0;
		};
		FReader Tracks(Chunks[1]);
		uint32 StoredTrackCount = 0;
		if (!Tracks.ReadU32(StoredTrackCount) || StoredTrackCount != TrackCount
			|| Tracks.Remaining() != static_cast<uint64>(TrackCount) * 20)
			return {EPayloadDecodeError::Corrupt, "Animation track chunk size is invalid."};
		std::vector<FTrackRecord> Records(TrackCount);
		uint64 ExpectedFirst = 0;
		for (FTrackRecord& Record : Records)
		{
			uint32 Reserved = 0;
			if (!Tracks.ReadU16(Record.BoneIndex) || !Tracks.ReadU8(Record.Path)
				|| !Tracks.ReadU8(Record.Interpolation) || !Tracks.ReadU64(Record.FirstKey)
				|| !Tracks.ReadU32(Record.KeyCount) || !Tracks.ReadU32(Reserved)
				|| Reserved != 0 || Record.KeyCount == 0 || Record.FirstKey != ExpectedFirst
				|| Record.KeyCount > KeyCount - std::min(KeyCount, Record.FirstKey))
				return {EPayloadDecodeError::Corrupt, "Animation track record is invalid."};
			ExpectedFirst += Record.KeyCount;
		}
		if (ExpectedFirst != KeyCount)
			return {EPayloadDecodeError::Corrupt, "Animation tracks do not partition the key arrays."};

		FReader Times(Chunks[2]);
		uint64 StoredTimeCount = 0;
		if (!Times.ReadU64(StoredTimeCount) || StoredTimeCount != KeyCount
			|| Times.Remaining() != KeyCount * sizeof(float))
			return {EPayloadDecodeError::Corrupt, "Animation time chunk size is invalid."};
		std::vector<float> AllTimes(static_cast<size_t>(KeyCount));
		for (float& Time : AllTimes)
			if (!Times.ReadFloat(Time))
				return {EPayloadDecodeError::Corrupt, "Animation time chunk is truncated."};

		FReader Values(Chunks[3]);
		uint64 StoredValueCount = 0;
		if (!Values.ReadU64(StoredValueCount) || StoredValueCount != KeyCount
			|| KeyCount > (std::numeric_limits<uint64>::max() - 8) / (4 * sizeof(float))
			|| Values.Remaining() != KeyCount * 4 * sizeof(float))
			return {EPayloadDecodeError::Corrupt, "Animation value chunk size is invalid."};
		std::vector<FVector4f> AllValues(static_cast<size_t>(KeyCount));
		for (FVector4f& Value : AllValues)
			if (!ReadVector(Values, Value))
				return {EPayloadDecodeError::Corrupt, "Animation value chunk is truncated."};

		Candidate.Tracks.reserve(TrackCount);
		for (const FTrackRecord& Record : Records)
		{
			FAnimationTrackData Track{
				.BoneIndex = Record.BoneIndex,
				.Path = static_cast<EAnimationTrackPath>(Record.Path),
				.Interpolation = static_cast<EAnimationInterpolation>(Record.Interpolation)};
			const size_t Begin = static_cast<size_t>(Record.FirstKey);
			const size_t End = Begin + Record.KeyCount;
			Track.Times.assign(AllTimes.begin() + Begin, AllTimes.begin() + End);
			if (Track.Path == EAnimationTrackPath::Rotation)
				Track.RotationValues.assign(AllValues.begin() + Begin, AllValues.begin() + End);
			else
			{
				Track.VectorValues.reserve(Record.KeyCount);
				for (size_t Index = Begin; Index < End; ++Index)
				{
					if (AllValues[Index].w != 0.0f)
						return {EPayloadDecodeError::Corrupt, "Animation vector key reserved component is nonzero."};
					Track.VectorValues.emplace_back(
						AllValues[Index].x, AllValues[Index].y, AllValues[Index].z);
				}
			}
			Candidate.Tracks.push_back(std::move(Track));
		}
		if (!ValidateAnimationClipPayload(Candidate, Context.SkeletonBoneCount, Error))
			return {EPayloadDecodeError::Corrupt, std::move(Error)};
		OutPayload = std::move(Candidate);
		return {};
	}

	auto FSkeletalMeshPayloadData::Serialize(
		FArchive& Ar,
		const FSkeletalPayloadSerializationContext& Context) -> void
	{
		if (Ar.IsSaving())
		{
			std::vector<uint8> Bytes;
			std::string Error;
			if (!BuildSkeletalMeshSerializedValue(
				*this, Context, Bytes, Error))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, Error);
				return;
			}
			Ar.Serialize(Bytes.data(), Bytes.size());
			return;
		}
		const uint64 ByteCount = Ar.GetRemainingPayloadBytes();
		if (ByteCount > MaximumSkeletalMeshPayloadBytes
			|| ByteCount > std::numeric_limits<size_t>::max())
		{
			Ar.Fail(EArchiveFailureCode::LimitExceeded,
				"SkeletalMesh payload exceeds the runtime byte limit.");
			return;
		}
		std::vector<uint8> Bytes(static_cast<size_t>(ByteCount));
		Ar.Serialize(Bytes.data(), Bytes.size());
		if (Ar.HasError()) return;
		FSkeletalMeshPayloadData Candidate;
		const FPayloadDecodeResult Result = ParseSkeletalMeshSerializedValue(
			Bytes, Context, Candidate);
		if (!Result)
		{
			Ar.Fail(Result.Code == EPayloadDecodeError::Incompatible
				? EArchiveFailureCode::UnsupportedVersion
				: EArchiveFailureCode::InvalidData,
				Result.Message);
			return;
		}
		*this = std::move(Candidate);
	}

	auto FAnimationClipPayloadData::Serialize(
		FArchive& Ar,
		const FSkeletalPayloadSerializationContext& Context) -> void
	{
		if (Ar.IsSaving())
		{
			std::vector<uint8> Bytes;
			std::string Error;
			if (!BuildAnimationClipSerializedValue(
				*this, Context, Bytes, Error))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, Error);
				return;
			}
			Ar.Serialize(Bytes.data(), Bytes.size());
			return;
		}
		const uint64 ByteCount = Ar.GetRemainingPayloadBytes();
		if (ByteCount > MaximumAnimationClipPayloadBytes
			|| ByteCount > std::numeric_limits<size_t>::max())
		{
			Ar.Fail(EArchiveFailureCode::LimitExceeded,
				"AnimationClip payload exceeds the runtime byte limit.");
			return;
		}
		std::vector<uint8> Bytes(static_cast<size_t>(ByteCount));
		Ar.Serialize(Bytes.data(), Bytes.size());
		if (Ar.HasError()) return;
		FAnimationClipPayloadData Candidate;
		const FPayloadDecodeResult Result = ParseAnimationClipSerializedValue(
			Bytes, Context, Candidate);
		if (!Result)
		{
			Ar.Fail(Result.Code == EPayloadDecodeError::Incompatible
				? EArchiveFailureCode::UnsupportedVersion
				: EArchiveFailureCode::InvalidData,
				Result.Message);
			return;
		}
		*this = std::move(Candidate);
	}
}
