#include "Terrain/TerrainHeightmapDerivedData.h"

#include "Misc/DerivedDataCache.h"
#include "Serialization/Archive.h"
#include "Terrain/TerrainHeightmap.h"

namespace Durin
{
	namespace
	{
		auto Fail(std::string& OutError, std::string Message) -> bool
		{
			OutError = std::move(Message);
			return false;
		}

		auto IsSupportedTarget(
			Asset::ECookTargetPlatform Platform,
			Asset::ECookTargetProfile Profile) -> bool
		{
			return Platform == Asset::ECookTargetPlatform::Win64
				&& (Profile == Asset::ECookTargetProfile::Game
					|| Profile == Asset::ECookTargetProfile::EditorValidation);
		}

		auto Align(uint64 Value) -> uint64
		{
			return (Value + TerrainHeightmapPayloadAlignment - 1)
				& ~(static_cast<uint64>(TerrainHeightmapPayloadAlignment) - 1);
		}

		auto WriteU16(DerivedDataCache::FWriter& Writer, uint16 Value) -> void
		{
			Writer.WriteU8(static_cast<uint8>(Value));
			Writer.WriteU8(static_cast<uint8>(Value >> 8));
		}

		auto ReadU16At(std::span<const uint8> Bytes, uint64 Offset, uint16& OutValue) -> bool
		{
			if (Offset > Bytes.size() || Bytes.size() - static_cast<size_t>(Offset) < 2) return false;
			OutValue = static_cast<uint16>(Bytes[static_cast<size_t>(Offset)])
				| static_cast<uint16>(Bytes[static_cast<size_t>(Offset) + 1]) << 8;
			return true;
		}

		auto ReadU32At(std::span<const uint8> Bytes, uint64 Offset, uint32& OutValue) -> bool
		{
			if (Offset > Bytes.size() || Bytes.size() - static_cast<size_t>(Offset) < 4) return false;
			OutValue = 0;
			for (uint32 Index = 0; Index < 4; ++Index)
				OutValue |= static_cast<uint32>(Bytes[static_cast<size_t>(Offset) + Index]) << (Index * 8);
			return true;
		}

		auto ReadU64At(std::span<const uint8> Bytes, uint64 Offset, uint64& OutValue) -> bool
		{
			if (Offset > Bytes.size() || Bytes.size() - static_cast<size_t>(Offset) < 8) return false;
			OutValue = 0;
			for (uint32 Index = 0; Index < 8; ++Index)
				OutValue |= static_cast<uint64>(Bytes[static_cast<size_t>(Offset) + Index]) << (Index * 8);
			return true;
		}
	}

	auto BuildTerrainHeightmapSerializedValue(
		const FTerrainHeightmapPayload& Payload,
		Asset::ECookTargetPlatform TargetPlatform,
		Asset::ECookTargetProfile TargetProfile,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool
	{
		OutBytes.clear();
		OutError.clear();
		if (!IsSupportedTarget(TargetPlatform, TargetProfile))
			return Fail(OutError, "Terrain heightmap payload target is unsupported.");
		if (!Payload.IsValid())
			return Fail(OutError, "Terrain heightmap payload is not canonical.");

		const uint64 LevelBytes = static_cast<uint64>(Payload.Levels.size())
			* TerrainHeightmapLevelRecordSize;
		const uint64 SampleOffset = Align(TerrainHeightmapPayloadHeaderSize + LevelBytes);
		const uint64 SampleBytes = static_cast<uint64>(Payload.Samples.size()) * sizeof(uint16);
		const uint64 HierarchyOffset = Align(SampleOffset + SampleBytes);
		const uint64 HierarchyBytes = static_cast<uint64>(Payload.Nodes.size()) * 4;
		const uint64 StoredSize = HierarchyOffset + HierarchyBytes;
		if (StoredSize > MaximumTerrainHeightmapPayloadBytes
			|| HierarchyBytes > MaximumTerrainHeightmapHierarchyBytes)
			return Fail(OutError, "Terrain heightmap payload exceeds its frozen byte ceilings.");

		DerivedDataCache::FWriter Body;
		for (const FTerrainHeightmapLevel& Level : Payload.Levels)
		{
			Body.WriteU32(Level.Width);
			Body.WriteU32(Level.Height);
			Body.WriteU64(Level.NodeOffset);
			Body.WriteU32(Level.SampleRegionSize);
			Body.WriteU32(0);
		}
		Body.WriteBytes(std::vector<uint8>(
			static_cast<size_t>(SampleOffset - TerrainHeightmapPayloadHeaderSize - LevelBytes), 0));
		for (uint16 Sample : Payload.Samples) WriteU16(Body, Sample);
		Body.WriteBytes(std::vector<uint8>(
			static_cast<size_t>(HierarchyOffset - SampleOffset - SampleBytes), 0));
		for (const FTerrainHeightmapMinMaxNode& Node : Payload.Nodes)
		{
			WriteU16(Body, Node.Minimum);
			WriteU16(Body, Node.Maximum);
		}
		const std::vector<uint8> BodyBytes = Body.TakeBytes();

		DerivedDataCache::FWriter Writer;
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
		std::span<const uint8> Bytes,
		Asset::ECookTargetPlatform ExpectedPlatform,
		Asset::ECookTargetProfile ExpectedProfile,
		std::shared_ptr<const FTerrainHeightmapPayload>& OutPayload) -> FPayloadDecodeResult
	{
		OutPayload.reset();
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
		if (!ReadU32At(Bytes, 0, Magic) || !ReadU32At(Bytes, 4, Schema)
			|| !ReadU32At(Bytes, 8, Builder) || !ReadU32At(Bytes, 12, Platform)
			|| !ReadU32At(Bytes, 16, Profile) || !ReadU32At(Bytes, 20, Width)
			|| !ReadU32At(Bytes, 24, Height) || !ReadU32At(Bytes, 28, BaseRegion)
			|| !ReadU32At(Bytes, 32, LevelCount) || !ReadU32At(Bytes, 36, NodeCount)
			|| !ReadU32At(Bytes, 40, Minimum) || !ReadU32At(Bytes, 44, Maximum)
			|| !ReadU32At(Bytes, 48, HeaderSize) || !ReadU32At(Bytes, 52, LevelRecordSize)
			|| !ReadU64At(Bytes, 56, LevelOffset) || !ReadU64At(Bytes, 64, SampleOffset)
			|| !ReadU64At(Bytes, 72, HierarchyOffset) || !ReadU64At(Bytes, 80, StoredSize)
			|| !ReadU64At(Bytes, 88, StoredHash))
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
			if (!ReadU32At(Bytes, Offset, Level.Width)
				|| !ReadU32At(Bytes, Offset + 4, Level.Height)
				|| !ReadU64At(Bytes, Offset + 8, Level.NodeOffset)
				|| !ReadU32At(Bytes, Offset + 16, Level.SampleRegionSize)
				|| !ReadU32At(Bytes, Offset + 20, Reserved)
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
			if (!ReadU16At(Bytes, SampleOffset + Index * 2, Samples[static_cast<size_t>(Index)]))
				return Reject(EPayloadDecodeError::Corrupt, "Terrain heightmap samples are truncated.");
		std::shared_ptr<const FTerrainHeightmapPayload> Candidate;
		std::string BuildError;
		if (!BuildTerrainHeightmapPayload(Width, Height, Samples, Candidate, BuildError))
			return Reject(EPayloadDecodeError::Corrupt, std::move(BuildError));
		if (Candidate->Minimum != Minimum || Candidate->Maximum != Maximum
			|| Candidate->Levels != Levels || Candidate->Nodes.size() != NodeCount)
			return Reject(EPayloadDecodeError::Corrupt, "Terrain heightmap hierarchy metadata is inconsistent.");
		for (uint64 Index = 0; Index < NodeCount; ++Index)
		{
			uint16 NodeMinimum = 0;
			uint16 NodeMaximum = 0;
			if (!ReadU16At(Bytes, HierarchyOffset + Index * 4, NodeMinimum)
				|| !ReadU16At(Bytes, HierarchyOffset + Index * 4 + 2, NodeMaximum)
				|| Candidate->Nodes[static_cast<size_t>(Index)]
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
		if (Ar.HasError()) return;
		if (Ar.IsSaving())
		{
			std::vector<uint8> Bytes;
			std::string Error;
			if (!BuildTerrainHeightmapSerializedValue(
				*this, TargetPlatform, TargetProfile, Bytes, Error))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, Error);
				return;
			}
			Ar.WriteBytes(std::as_bytes(std::span<const uint8>(Bytes)));
			return;
		}

		const uint64 ByteCount = Ar.GetRemainingPayloadBytes();
		if (ByteCount == std::numeric_limits<uint64>::max())
		{
			Ar.Fail(EArchiveFailureCode::UnsupportedCapability,
				"Terrain heightmap payload requires a bounded input archive.");
			return;
		}
		if (ByteCount > MaximumTerrainHeightmapPayloadBytes
			|| ByteCount > static_cast<uint64>(std::vector<uint8>().max_size()))
		{
			Ar.Fail(EArchiveFailureCode::LimitExceeded,
				"Terrain heightmap payload exceeds its stored-size limit.");
			return;
		}
		std::vector<uint8> Bytes(static_cast<size_t>(ByteCount));
		Ar.ReadBytes(std::as_writable_bytes(std::span<uint8>(Bytes)));
		if (Ar.HasError()) return;
		std::shared_ptr<const FTerrainHeightmapPayload> Candidate;
		const FPayloadDecodeResult Result = ParseTerrainHeightmapSerializedValue(
			Bytes, TargetPlatform, TargetProfile, Candidate);
		if (!Result)
		{
			Ar.Fail(Result.Code == EPayloadDecodeError::Incompatible
				? EArchiveFailureCode::UnsupportedVersion : EArchiveFailureCode::InvalidData,
				Result.Message);
			return;
		}
		*this = *Candidate;
	}
}
