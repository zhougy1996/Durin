#include "Terrain/TerrainHeightmapDerivedData.h"

#include "Serialization/BinaryFormat.h"
#include "Serialization/Archive.h"
#include "Serialization/BoundedPayloadSerialization.h"
#include "Terrain/TerrainHeightmap.h"
#include "Templates/CheckedArithmetic.h"

namespace Durin
{
	namespace
	{
		auto IsSupportedTarget(
			Asset::ECookTargetPlatform Platform,
			Asset::ECookTargetProfile Profile) -> bool
		{
			return Platform == Asset::ECookTargetPlatform::Win64
				&& (Profile == Asset::ECookTargetProfile::Game
					|| Profile == Asset::ECookTargetProfile::EditorValidation);
		}

	}

	auto BuildTerrainHeightmapSerializedValue(
		const FTerrainHeightmapPayload& Payload,
		Asset::ECookTargetPlatform TargetPlatform,
		Asset::ECookTargetProfile TargetProfile,
		std::vector<std::byte>& OutBytes,
		std::string& OutError) -> bool
	{
		OutBytes.clear();
		OutError.clear();
		if (!IsSupportedTarget(TargetPlatform, TargetProfile))
			return Fail("Terrain heightmap payload target is unsupported.", &OutError);
		if (!Payload.IsValid())
			return Fail("Terrain heightmap payload is not canonical.", &OutError);

		uint64 LevelBytes = 0, LevelEnd = 0, SampleOffset = 0, SampleBytes = 0;
		uint64 SampleEnd = 0, HierarchyOffset = 0, HierarchyBytes = 0, StoredSize = 0;
		if (!TryMultiply(Payload.Levels.size(), TerrainHeightmapLevelRecordSize,
				MaximumTerrainHeightmapPayloadBytes, LevelBytes)
			|| !TryAdd(TerrainHeightmapPayloadHeaderSize, LevelBytes,
				MaximumTerrainHeightmapPayloadBytes, LevelEnd)
			|| !TryAlignUp(LevelEnd, TerrainHeightmapPayloadAlignment,
				MaximumTerrainHeightmapPayloadBytes, SampleOffset)
			|| !TryMultiply(Payload.Samples.size(), sizeof(uint16),
				MaximumTerrainHeightmapPayloadBytes, SampleBytes)
			|| !TryAdd(SampleOffset, SampleBytes, MaximumTerrainHeightmapPayloadBytes, SampleEnd)
			|| !TryAlignUp(SampleEnd, TerrainHeightmapPayloadAlignment,
				MaximumTerrainHeightmapPayloadBytes, HierarchyOffset)
			|| !TryMultiply(Payload.Nodes.size(), uint64{4},
				MaximumTerrainHeightmapHierarchyBytes, HierarchyBytes)
			|| !TryAdd(HierarchyOffset, HierarchyBytes,
				MaximumTerrainHeightmapPayloadBytes, StoredSize))
			return Fail("Terrain heightmap payload exceeds its frozen byte ceilings.", &OutError);

		FBinaryWriter Body;
		for (const FTerrainHeightmapLevel& Level : Payload.Levels)
		{
			Body.WriteU32(Level.Width);
			Body.WriteU32(Level.Height);
			Body.WriteU64(Level.NodeOffset);
			Body.WriteU32(Level.SampleRegionSize);
			Body.WriteU32(0);
		}
		Body.WriteBytes(std::vector<std::byte>(
			static_cast<size_t>(SampleOffset - TerrainHeightmapPayloadHeaderSize - LevelBytes), std::byte{0}));
		for (uint16 Sample : Payload.Samples) Body.WriteU16(Sample);
		Body.WriteBytes(std::vector<std::byte>(
			static_cast<size_t>(HierarchyOffset - SampleOffset - SampleBytes), std::byte{0}));
		for (const FTerrainHeightmapMinMaxNode& Node : Payload.Nodes)
		{
			Body.WriteU16(Node.Minimum);
			Body.WriteU16(Node.Maximum);
		}
		const std::vector<std::byte> BodyBytes = Body.TakeBytes();

		FBinaryWriter Writer;
		Writer.WriteU32(TerrainHeightmapPayloadMagic);
		Writer.WriteU32(TerrainHeightmapPayloadSchemaVersion);
		Writer.WriteU32(TerrainHeightmapBuilderVersion);
		Writer.WriteU32(static_cast<uint32>(TargetPlatform));
		Writer.WriteU32(static_cast<uint32>(TargetProfile));
		Writer.WriteU32(Payload.Width);
		Writer.WriteU32(Payload.Height);
		Writer.WriteU32(TerrainHeightmapBaseRegionSize);
		Writer.WriteU32(static_cast<uint32>(Payload.Levels.size()));
		Writer.WriteU32(static_cast<uint32>(Payload.Nodes.size()));
		Writer.WriteU32(Payload.Minimum);
		Writer.WriteU32(Payload.Maximum);
		Writer.WriteU32(TerrainHeightmapPayloadHeaderSize);
		Writer.WriteU32(TerrainHeightmapLevelRecordSize);
		Writer.WriteU64(TerrainHeightmapPayloadHeaderSize);
		Writer.WriteU64(SampleOffset);
		Writer.WriteU64(HierarchyOffset);
		Writer.WriteU64(StoredSize);
		Writer.WriteU64(FXxHash64::HashBuffer(BodyBytes).HashValue);
		Writer.WriteBytes(BodyBytes);
		OutBytes = Writer.TakeBytes();
		return true;
	}

	auto ParseTerrainHeightmapSerializedValue(
		std::span<const std::byte> Bytes,
		Asset::ECookTargetPlatform ExpectedPlatform,
		Asset::ECookTargetProfile ExpectedProfile,
		FTerrainHeightmapPayload& OutPayload) -> FPayloadDecodeResult
	{
		auto Reject = [](EPayloadDecodeError Code, std::string Message) {
			return FPayloadDecodeResult{.Code = Code, .Message = std::move(Message)};
		};
		if (!IsSupportedTarget(ExpectedPlatform, ExpectedProfile))
			return Reject(EPayloadDecodeError::Incompatible, "Terrain heightmap expected target is unsupported.");
		if (Bytes.size() < TerrainHeightmapPayloadHeaderSize
			|| Bytes.size() > MaximumTerrainHeightmapPayloadBytes)
			return Reject(EPayloadDecodeError::Corrupt, "Terrain heightmap payload size is invalid.");

		uint32 Magic = 0, Schema = 0, Builder = 0, Platform = 0, Profile = 0;
		uint32 Width = 0, Height = 0, BaseRegion = 0, LevelCount = 0, NodeCount = 0;
		uint32 Minimum = 0, Maximum = 0, HeaderSize = 0, LevelRecordSize = 0;
		uint64 LevelOffset = 0, SampleOffset = 0, HierarchyOffset = 0, StoredSize = 0, StoredHash = 0;
		if (!ReadLittleEndianAt(Bytes, 0, Magic) || !ReadLittleEndianAt(Bytes, 4, Schema)
			|| !ReadLittleEndianAt(Bytes, 8, Builder) || !ReadLittleEndianAt(Bytes, 12, Platform)
			|| !ReadLittleEndianAt(Bytes, 16, Profile) || !ReadLittleEndianAt(Bytes, 20, Width)
			|| !ReadLittleEndianAt(Bytes, 24, Height) || !ReadLittleEndianAt(Bytes, 28, BaseRegion)
			|| !ReadLittleEndianAt(Bytes, 32, LevelCount) || !ReadLittleEndianAt(Bytes, 36, NodeCount)
			|| !ReadLittleEndianAt(Bytes, 40, Minimum) || !ReadLittleEndianAt(Bytes, 44, Maximum)
			|| !ReadLittleEndianAt(Bytes, 48, HeaderSize) || !ReadLittleEndianAt(Bytes, 52, LevelRecordSize)
			|| !ReadLittleEndianAt(Bytes, 56, LevelOffset) || !ReadLittleEndianAt(Bytes, 64, SampleOffset)
			|| !ReadLittleEndianAt(Bytes, 72, HierarchyOffset) || !ReadLittleEndianAt(Bytes, 80, StoredSize)
			|| !ReadLittleEndianAt(Bytes, 88, StoredHash))
			return Reject(EPayloadDecodeError::Corrupt, "Terrain heightmap payload header is truncated.");
		if (Magic != TerrainHeightmapPayloadMagic)
			return Reject(EPayloadDecodeError::Corrupt, "Terrain heightmap payload magic is invalid.");
		if (Schema != TerrainHeightmapPayloadSchemaVersion || Builder != TerrainHeightmapBuilderVersion)
			return Reject(EPayloadDecodeError::Incompatible, "Terrain heightmap payload schema or builder is unsupported.");
		if (Platform != static_cast<uint32>(ExpectedPlatform)
			|| Profile != static_cast<uint32>(ExpectedProfile))
			return Reject(EPayloadDecodeError::Incompatible, "Terrain heightmap payload target does not match.");
		if (HeaderSize != TerrainHeightmapPayloadHeaderSize
			|| LevelRecordSize != TerrainHeightmapLevelRecordSize
			|| LevelOffset != TerrainHeightmapPayloadHeaderSize || LevelCount == 0
			|| Minimum > Maximum || Maximum > 65535 || StoredSize != Bytes.size())
			return Reject(EPayloadDecodeError::Corrupt, "Terrain heightmap payload header facts are invalid.");
		const uint64 LevelEnd = LevelOffset + static_cast<uint64>(LevelCount) * LevelRecordSize;
		const uint64 SampleCount = static_cast<uint64>(Width) * Height;
		const uint64 SampleBytes = SampleCount * sizeof(uint16);
		const uint64 HierarchyBytes = static_cast<uint64>(NodeCount) * 4;
		if (Width < 2 || Height < 2 || Width > MaximumTerrainHeightmapDimension
			|| Height > MaximumTerrainHeightmapDimension || SampleCount > MaximumTerrainHeightmapSamples
			|| LevelEnd > SampleOffset || SampleOffset % TerrainHeightmapPayloadAlignment != 0
			|| SampleOffset > HierarchyOffset || SampleBytes > HierarchyOffset - SampleOffset
			|| HierarchyOffset % TerrainHeightmapPayloadAlignment != 0
			|| HierarchyBytes > MaximumTerrainHeightmapHierarchyBytes
			|| HierarchyOffset > StoredSize || HierarchyBytes != StoredSize - HierarchyOffset)
			return Reject(EPayloadDecodeError::Corrupt, "Terrain heightmap payload ranges or ceilings are invalid.");
		if (FXxHash64::HashBuffer(Bytes.subspan(TerrainHeightmapPayloadHeaderSize)).HashValue != StoredHash)
			return Reject(EPayloadDecodeError::Corrupt, "Terrain heightmap payload checksum does not match.");

		std::vector<FTerrainHeightmapLevel> Levels;
		Levels.reserve(LevelCount);
		uint64 ExpectedNodeOffset = 0;
		for (uint32 Index = 0; Index < LevelCount; ++Index)
		{
			const uint64 Offset = LevelOffset + static_cast<uint64>(Index) * LevelRecordSize;
			FTerrainHeightmapLevel Level;
			uint32 Reserved = 0;
			if (!ReadLittleEndianAt(Bytes, Offset, Level.Width)
				|| !ReadLittleEndianAt(Bytes, Offset + 4, Level.Height)
				|| !ReadLittleEndianAt(Bytes, Offset + 8, Level.NodeOffset)
				|| !ReadLittleEndianAt(Bytes, Offset + 16, Level.SampleRegionSize)
				|| !ReadLittleEndianAt(Bytes, Offset + 20, Reserved)
				|| Reserved != 0 || Level.Width == 0 || Level.Height == 0
				|| Level.NodeOffset != ExpectedNodeOffset)
				return Reject(EPayloadDecodeError::Corrupt, "Terrain heightmap level table is invalid.");
			const uint64 Count = static_cast<uint64>(Level.Width) * Level.Height;
			if (Count > NodeCount - ExpectedNodeOffset)
				return Reject(EPayloadDecodeError::Corrupt, "Terrain heightmap level nodes overflow the hierarchy.");
			ExpectedNodeOffset += Count;
			Levels.push_back(Level);
		}
		if (ExpectedNodeOffset != NodeCount)
			return Reject(EPayloadDecodeError::Corrupt, "Terrain heightmap node count does not match its levels.");

		std::vector<uint16> Samples(static_cast<size_t>(SampleCount));
		for (uint64 Index = 0; Index < SampleCount; ++Index)
			if (!ReadLittleEndianAt(Bytes, SampleOffset + Index * 2, Samples[static_cast<size_t>(Index)]))
				return Reject(EPayloadDecodeError::Corrupt, "Terrain heightmap samples are truncated.");
		std::shared_ptr<const FTerrainHeightmapPayload> BuiltCandidate;
		std::string BuildError;
		if (!BuildTerrainHeightmapPayload(Width, Height, Samples, BuiltCandidate, BuildError))
			return Reject(EPayloadDecodeError::Corrupt, std::move(BuildError));
		FTerrainHeightmapPayload Candidate = *BuiltCandidate;
		if (Candidate.Minimum != Minimum || Candidate.Maximum != Maximum
			|| Candidate.Levels != Levels || Candidate.Nodes.size() != NodeCount)
			return Reject(EPayloadDecodeError::Corrupt, "Terrain heightmap hierarchy metadata is inconsistent.");
		for (uint64 Index = 0; Index < NodeCount; ++Index)
		{
			uint16 NodeMinimum = 0;
			uint16 NodeMaximum = 0;
			if (!ReadLittleEndianAt(Bytes, HierarchyOffset + Index * 4, NodeMinimum)
				|| !ReadLittleEndianAt(Bytes, HierarchyOffset + Index * 4 + 2, NodeMaximum)
				|| Candidate.Nodes[static_cast<size_t>(Index)]
					!= FTerrainHeightmapMinMaxNode{NodeMinimum, NodeMaximum})
				return Reject(EPayloadDecodeError::Corrupt, "Terrain heightmap hierarchy extrema are inconsistent.");
		}
		OutPayload = std::move(Candidate);
		return {};
	}

	auto FTerrainHeightmapPayload::Serialize(
		FArchive& Ar,
		Asset::ECookTargetPlatform TargetPlatform,
		Asset::ECookTargetProfile TargetProfile) -> void
	{
		SerializeBoundedArchivePayload(
			Ar,
			*this,
			{MaximumTerrainHeightmapPayloadBytes, "Terrain heightmap payload"},
			[&](const FTerrainHeightmapPayload& Value,
				std::vector<std::byte>& Bytes, std::string& Error) {
				return BuildTerrainHeightmapSerializedValue(
					Value, TargetPlatform, TargetProfile, Bytes, Error);
			},
			[&](std::span<const std::byte> Bytes, FTerrainHeightmapPayload& Candidate) {
				return ParseTerrainHeightmapSerializedValue(
					Bytes, TargetPlatform, TargetProfile, Candidate);
			});
	}
}
