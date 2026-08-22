#include "StaticMesh/StaticMeshDerivedData.h"

#include "Asset/ChunkedPayload.h"
#include "Serialization/EngineWire.h"
#include "Serialization/Archive.h"
#include "Serialization/BoundedPayloadSerialization.h"



namespace Durin
{
	namespace
	{
		inline constexpr uint32 StaticMeshPayloadRequiredChunkCount = 6;
		inline constexpr uint32 StaticMeshPayloadFlagCompressed = 1;
		inline constexpr uint32 StaticMeshChunkCompressionMask = 0x0000ff00;
		inline constexpr uint32 StaticMeshChunkCompressionZstandard = 1;
		inline constexpr uint64 StaticMeshMaximumCompressionRatio = 64;

		using FPayloadWriter = EngineWire::FWriter;
		using FPayloadReader = EngineWire::FReader;

		auto IsFinite(const FVector2f& Value) -> bool
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y);
		}

		auto IsFinite(const FVector3f& Value) -> bool
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}

		auto IsFinite(const FVector4f& Value) -> bool
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y)
				&& std::isfinite(Value.z) && std::isfinite(Value.w);
		}

		auto IsValidBounds(const FBox& Bounds) -> bool
		{
			return Bounds.bIsValid
				&& std::isfinite(Bounds.Min.x) && std::isfinite(Bounds.Min.y) && std::isfinite(Bounds.Min.z)
				&& std::isfinite(Bounds.Max.x) && std::isfinite(Bounds.Max.y) && std::isfinite(Bounds.Max.z)
				&& Bounds.Min.x <= Bounds.Max.x && Bounds.Min.y <= Bounds.Max.y && Bounds.Min.z <= Bounds.Max.z
				&& static_cast<double>(static_cast<float>(Bounds.Min.x)) == Bounds.Min.x
				&& static_cast<double>(static_cast<float>(Bounds.Min.y)) == Bounds.Min.y
				&& static_cast<double>(static_cast<float>(Bounds.Min.z)) == Bounds.Min.z
				&& static_cast<double>(static_cast<float>(Bounds.Max.x)) == Bounds.Max.x
				&& static_cast<double>(static_cast<float>(Bounds.Max.y)) == Bounds.Max.y
				&& static_cast<double>(static_cast<float>(Bounds.Max.z)) == Bounds.Max.z;
		}

		auto ValidatePayload(const FStaticMeshPayloadData& Payload, std::string& OutError) -> bool
		{
			if (!IsValidBounds(Payload.LocalBounds)) return Fail("Static-mesh payload bounds are invalid or not exactly representable as float32.", &OutError);
			if (Payload.MaterialSlotCount == 0 || Payload.MaterialSlotCount > MaximumStaticMeshMaterialSlots)
				return Fail("Static-mesh payload material-slot count is outside the supported range.", &OutError);
			if (Payload.LODs.empty() || Payload.LODs.size() > MaximumStaticMeshLODs)
				return Fail("Static-mesh payload LOD count is outside the supported range.", &OutError);

			uint64 EncodedSizeUpperBound = StaticMeshPayloadHeaderSize
				+ StaticMeshPayloadRequiredChunkCount * StaticMeshPayloadChunkEntrySize
				+ StaticMeshPayloadRequiredChunkCount * (StaticMeshPayloadAlignment - 1)
				+ 24ull
				+ 4ull
				+ 4ull + static_cast<uint64>(Payload.LODs.size()) * 44ull;
			for (size_t LODIndex = 0; LODIndex < Payload.LODs.size(); ++LODIndex)
			{
				const FStaticMeshPayloadLOD& LOD = Payload.LODs[LODIndex];
				if (!std::isfinite(LOD.ScreenSize)
					|| LOD.ScreenSize < 0.0f || LOD.ScreenSize > 1.0f
					|| (LOD.ScreenSize == 0.0f
						&& std::signbit(LOD.ScreenSize)))
				{
					return Fail(std::format(
						"Static-mesh payload LOD {} screen size must be finite and in [0, 1].",
						LODIndex), &OutError);
				}
				if (LODIndex > 0
					&& LOD.ScreenSize >= Payload.LODs[LODIndex - 1].ScreenSize)
				{
					return Fail("Static-mesh payload LOD screen sizes must be strictly descending.", &OutError);
				}
				const size_t VertexCount = LOD.Positions.size();
				const size_t IndexCount = LOD.Indices.size();
				if (VertexCount == 0 || VertexCount > MaximumStaticMeshVerticesPerLOD)
					return Fail(std::format("Static-mesh payload LOD {} has an invalid vertex count.", LODIndex), &OutError);
				if (IndexCount == 0 || IndexCount > MaximumStaticMeshIndicesPerLOD)
					return Fail(std::format("Static-mesh payload LOD {} has an invalid index count.", LODIndex), &OutError);
				if (LOD.Sections.empty() || LOD.Sections.size() > MaximumStaticMeshSectionsPerLOD)
					return Fail(std::format("Static-mesh payload LOD {} has an invalid section count.", LODIndex), &OutError);
				if (LOD.NumTexCoords > MaxStaticMeshUVChannels)
					return Fail(std::format("Static-mesh payload LOD {} has an invalid UV-channel count.", LODIndex), &OutError);
				const uint64 LODPayloadBytes = 4ull + static_cast<uint64>(LOD.Sections.size()) * 44ull
					+ static_cast<uint64>(VertexCount) * (40ull + static_cast<uint64>(LOD.NumTexCoords) * 8ull
						+ (LOD.bHasVertexColors ? 16ull : 0ull))
					+ static_cast<uint64>(IndexCount) * 4ull;
				if (LODPayloadBytes > MaximumStaticMeshPayloadBytes - EncodedSizeUpperBound)
					return Fail("Static-mesh payload exceeds the stored-object size limit.", &OutError);
				EncodedSizeUpperBound += LODPayloadBytes;
				if (!IsValidBounds(LOD.LocalBounds))
					return Fail(std::format("Static-mesh payload LOD {} bounds are invalid.", LODIndex), &OutError);
				if (LOD.Normals.size() != VertexCount || LOD.Tangents.size() != VertexCount)
					return Fail(std::format("Static-mesh payload LOD {} vertex-stream counts do not match.", LODIndex), &OutError);
				for (uint32 Channel = 0; Channel < MaxStaticMeshUVChannels; ++Channel)
				{
					const size_t ExpectedCount = Channel < LOD.NumTexCoords ? VertexCount : 0;
					if (LOD.TexCoords[Channel].size() != ExpectedCount)
						return Fail(std::format(
							"Static-mesh payload LOD {} UV stream {} has {} values; expected {}.",
							LODIndex, Channel, LOD.TexCoords[Channel].size(), ExpectedCount), &OutError);
				}
				if (LOD.Colors.size() != (LOD.bHasVertexColors ? VertexCount : 0))
					return Fail(std::format("Static-mesh payload LOD {} color-stream count does not match its flags.", LODIndex), &OutError);
				if (std::ranges::any_of(LOD.Positions, [](const FVector3f& Value) { return !IsFinite(Value); })
					|| std::ranges::any_of(LOD.Normals, [](const FVector3f& Value) { return !IsFinite(Value); })
					|| std::ranges::any_of(LOD.Tangents, [](const FVector4f& Value) { return !IsFinite(Value); })
					|| std::ranges::any_of(LOD.Colors, [](const FVector4f& Value) { return !IsFinite(Value); }))
					return Fail(std::format("Static-mesh payload LOD {} contains a non-finite vertex attribute.", LODIndex), &OutError);
				for (uint32 Channel = 0; Channel < LOD.NumTexCoords; ++Channel)
				{
					if (std::ranges::any_of(LOD.TexCoords[Channel], [](const FVector2f& Value) { return !IsFinite(Value); }))
						return Fail(std::format("Static-mesh payload LOD {} contains a non-finite UV.", LODIndex), &OutError);
				}
				if (std::ranges::any_of(LOD.Indices, [VertexCount](uint32 Index) { return Index >= VertexCount; }))
					return Fail(std::format("Static-mesh payload LOD {} contains an out-of-range index.", LODIndex), &OutError);

				uint64 CoveredIndices = 0;
				for (const FStaticMeshPayloadSection& Section : LOD.Sections)
				{
					const uint64 SectionEnd = static_cast<uint64>(Section.FirstIndex) + Section.IndexCount;
					if (Section.IndexCount == 0 || Section.FirstIndex != CoveredIndices || SectionEnd > IndexCount)
						return Fail(std::format("Static-mesh payload LOD {} sections do not exactly cover its index buffer.", LODIndex), &OutError);
					if (Section.MinVertexIndex > Section.MaxVertexIndex || Section.MaxVertexIndex >= VertexCount)
						return Fail(std::format("Static-mesh payload LOD {} section has an invalid vertex range.", LODIndex), &OutError);
					if (Section.MaterialSlotIndex >= Payload.MaterialSlotCount)
						return Fail(std::format("Static-mesh payload LOD {} section has an invalid material slot.", LODIndex), &OutError);
					if (!IsValidBounds(Section.LocalBounds))
						return Fail(std::format("Static-mesh payload LOD {} section bounds are invalid.", LODIndex), &OutError);

					uint32 ActualMinimum = std::numeric_limits<uint32>::max();
					uint32 ActualMaximum = 0;
					for (uint64 IndexOffset = Section.FirstIndex; IndexOffset < SectionEnd; ++IndexOffset)
					{
						ActualMinimum = std::min(ActualMinimum, LOD.Indices[static_cast<size_t>(IndexOffset)]);
						ActualMaximum = std::max(ActualMaximum, LOD.Indices[static_cast<size_t>(IndexOffset)]);
					}
					if (ActualMinimum != Section.MinVertexIndex || ActualMaximum != Section.MaxVertexIndex)
						return Fail(std::format("Static-mesh payload LOD {} section vertex range does not match its indices.", LODIndex), &OutError);
					CoveredIndices = SectionEnd;
				}
				if (CoveredIndices != IndexCount)
					return Fail(std::format("Static-mesh payload LOD {} sections do not cover its complete index buffer.", LODIndex), &OutError);
			}
			if (Payload.LODs.back().ScreenSize != 0.0f)
				return Fail("Static-mesh payload lowest-detail LOD screen size must be exactly zero.", &OutError);
			return true;
		}

		auto WriteBounds(FPayloadWriter& Writer, const FBox& Bounds) -> void
		{
			Writer.WriteFloat(static_cast<float>(Bounds.Min.x));
			Writer.WriteFloat(static_cast<float>(Bounds.Min.y));
			Writer.WriteFloat(static_cast<float>(Bounds.Min.z));
			Writer.WriteFloat(static_cast<float>(Bounds.Max.x));
			Writer.WriteFloat(static_cast<float>(Bounds.Max.y));
			Writer.WriteFloat(static_cast<float>(Bounds.Max.z));
		}

		auto ReadBounds(FPayloadReader& Reader, FBox& Bounds) -> bool
		{
			std::array<float, 6> Values{};
			for (float& Value : Values)
				if (!Reader.ReadFloat(Value) || !std::isfinite(Value)) return false;
			Bounds = FBox(
				FVector3(Values[0], Values[1], Values[2]),
				FVector3(Values[3], Values[4], Values[5]));
			return Bounds.bIsValid;
		}

		auto BuildPayloadChunks(const FStaticMeshPayloadData& Payload) -> std::array<std::vector<uint8>, StaticMeshPayloadRequiredChunkCount>
		{
			std::array<std::vector<uint8>, StaticMeshPayloadRequiredChunkCount> Chunks;

			FPayloadWriter Bounds;
			WriteBounds(Bounds, Payload.LocalBounds);
			Chunks[0] = Bounds.TakeBytes();

			FPayloadWriter MaterialSlots;
			MaterialSlots.WriteU32(Payload.MaterialSlotCount);
			Chunks[1] = MaterialSlots.TakeBytes();

			FPayloadWriter LODs;
			LODs.WriteU32(static_cast<uint32>(Payload.LODs.size()));
			for (const FStaticMeshPayloadLOD& LOD : Payload.LODs)
			{
				LODs.WriteU32(static_cast<uint32>(LOD.Positions.size()));
				LODs.WriteU32(static_cast<uint32>(LOD.Indices.size()));
				LODs.WriteU32(static_cast<uint32>(LOD.Sections.size()));
				LODs.WriteU8(LOD.NumTexCoords);
				LODs.WriteU8(LOD.bHasVertexColors ? 1 : 0);
				LODs.WriteU16(0);
				LODs.WriteFloat(LOD.ScreenSize);
				WriteBounds(LODs, LOD.LocalBounds);
			}
			Chunks[2] = LODs.TakeBytes();

			FPayloadWriter Sections;
			for (const FStaticMeshPayloadLOD& LOD : Payload.LODs)
			{
				Sections.WriteU32(static_cast<uint32>(LOD.Sections.size()));
				for (const FStaticMeshPayloadSection& Section : LOD.Sections)
				{
					Sections.WriteU32(Section.FirstIndex);
					Sections.WriteU32(Section.IndexCount);
					Sections.WriteU32(Section.MinVertexIndex);
					Sections.WriteU32(Section.MaxVertexIndex);
					Sections.WriteU32(Section.MaterialSlotIndex);
					WriteBounds(Sections, Section.LocalBounds);
				}
			}
			Chunks[3] = Sections.TakeBytes();

			FPayloadWriter VertexStreams;
			for (const FStaticMeshPayloadLOD& LOD : Payload.LODs)
			{
				for (const FVector3f& Value : LOD.Positions)
					for (uint32 Component = 0; Component < 3; ++Component) VertexStreams.WriteFloat(Value[Component]);
				for (const FVector3f& Value : LOD.Normals)
					for (uint32 Component = 0; Component < 3; ++Component) VertexStreams.WriteFloat(Value[Component]);
				for (const FVector4f& Value : LOD.Tangents)
					for (uint32 Component = 0; Component < 4; ++Component) VertexStreams.WriteFloat(Value[Component]);
				for (uint32 Channel = 0; Channel < LOD.NumTexCoords; ++Channel)
					for (const FVector2f& Value : LOD.TexCoords[Channel])
						for (uint32 Component = 0; Component < 2; ++Component) VertexStreams.WriteFloat(Value[Component]);
				if (LOD.bHasVertexColors)
					for (const FVector4f& Value : LOD.Colors)
						for (uint32 Component = 0; Component < 4; ++Component) VertexStreams.WriteFloat(Value[Component]);
			}
			Chunks[4] = VertexStreams.TakeBytes();

			FPayloadWriter IndexBuffers;
			for (const FStaticMeshPayloadLOD& LOD : Payload.LODs)
				for (uint32 Index : LOD.Indices) IndexBuffers.WriteU32(Index);
			Chunks[5] = IndexBuffers.TakeBytes();
			return Chunks;
		}

		auto ReadPayloadChunks(
			const std::array<std::span<const uint8>, StaticMeshPayloadRequiredChunkCount>& Chunks,
			FStaticMeshPayloadData& OutPayload,
			std::string& OutError) -> bool
		{
			FStaticMeshPayloadData Payload;

			FPayloadReader Bounds(Chunks[0]);
			if (!ReadBounds(Bounds, Payload.LocalBounds) || !Bounds.IsAtEnd())
				return Fail("Static-mesh bounds chunk is malformed.", &OutError);

			FPayloadReader MaterialSlots(Chunks[1]);
			uint32 MaterialSlotCount = 0;
			if (!MaterialSlots.ReadU32(MaterialSlotCount) || MaterialSlotCount == 0 || MaterialSlotCount > MaximumStaticMeshMaterialSlots)
				return Fail("Static-mesh material-slot chunk has an invalid count.", &OutError);
			if (Chunks[1].size() != 4ull)
				return Fail("Static-mesh material-slot chunk has an invalid size.", &OutError);
			Payload.MaterialSlotCount = MaterialSlotCount;

			FPayloadReader LODs(Chunks[2]);
			uint32 LODCount = 0;
			if (!LODs.ReadU32(LODCount) || LODCount == 0 || LODCount > MaximumStaticMeshLODs)
				return Fail("Static-mesh LOD chunk has an invalid count.", &OutError);
			if (Chunks[2].size() != 4ull + static_cast<uint64>(LODCount) * 44ull)
				return Fail("Static-mesh LOD chunk has an invalid size.", &OutError);
			Payload.LODs.resize(LODCount);
			std::vector<uint32> VertexCounts(LODCount);
			std::vector<uint32> IndexCounts(LODCount);
			std::vector<uint32> SectionCounts(LODCount);
			uint64 ExpectedSectionBytes = 0;
			uint64 ExpectedVertexBytes = 0;
			uint64 ExpectedIndexBytes = 0;
			for (uint32 LODIndex = 0; LODIndex < LODCount; ++LODIndex)
			{
				FStaticMeshPayloadLOD& LOD = Payload.LODs[LODIndex];
				uint32& VertexCount = VertexCounts[LODIndex];
				uint32& IndexCount = IndexCounts[LODIndex];
				uint32& SectionCount = SectionCounts[LODIndex];
				uint8 Flags = 0;
				uint16 Reserved = 0;
				if (!LODs.ReadU32(VertexCount) || !LODs.ReadU32(IndexCount) || !LODs.ReadU32(SectionCount)
					|| !LODs.ReadU8(LOD.NumTexCoords) || !LODs.ReadU8(Flags) || !LODs.ReadU16(Reserved)
					|| !LODs.ReadFloat(LOD.ScreenSize)
					|| !ReadBounds(LODs, LOD.LocalBounds))
					return Fail("Static-mesh LOD chunk is truncated.", &OutError);
				if (VertexCount == 0 || VertexCount > MaximumStaticMeshVerticesPerLOD
					|| IndexCount == 0 || IndexCount > MaximumStaticMeshIndicesPerLOD
					|| SectionCount == 0 || SectionCount > MaximumStaticMeshSectionsPerLOD
					|| LOD.NumTexCoords > MaxStaticMeshUVChannels
					|| (Flags & ~1u) != 0 || Reserved != 0)
					return Fail("Static-mesh LOD chunk contains an invalid count, flag, or reserved value.", &OutError);
				LOD.bHasVertexColors = (Flags & 1u) != 0;

				const uint64 VertexStride = 40ull + static_cast<uint64>(LOD.NumTexCoords) * 8ull
					+ (LOD.bHasVertexColors ? 16ull : 0ull);
				const uint64 SectionBytes = 4ull + static_cast<uint64>(SectionCount) * 44ull;
				const uint64 VertexBytes = static_cast<uint64>(VertexCount) * VertexStride;
				const uint64 IndexBytes = static_cast<uint64>(IndexCount) * 4ull;
				if (SectionBytes > MaximumStaticMeshPayloadBytes - ExpectedSectionBytes
					|| VertexBytes > MaximumStaticMeshPayloadBytes - ExpectedVertexBytes
					|| IndexBytes > MaximumStaticMeshPayloadBytes - ExpectedIndexBytes)
					return Fail("Static-mesh payload stream sizes exceed the allocation limit.", &OutError);
				ExpectedSectionBytes += SectionBytes;
				ExpectedVertexBytes += VertexBytes;
				ExpectedIndexBytes += IndexBytes;
			}
			if (Chunks[3].size() != ExpectedSectionBytes || Chunks[4].size() != ExpectedVertexBytes
				|| Chunks[5].size() != ExpectedIndexBytes)
				return Fail("Static-mesh payload stream chunk sizes do not match the LOD metadata.", &OutError);
			for (uint32 LODIndex = 0; LODIndex < LODCount; ++LODIndex)
			{
				FStaticMeshPayloadLOD& LOD = Payload.LODs[LODIndex];
				const uint32 VertexCount = VertexCounts[LODIndex];
				LOD.Positions.resize(VertexCount);
				LOD.Normals.resize(VertexCount);
				LOD.Tangents.resize(VertexCount);
				for (uint32 Channel = 0; Channel < LOD.NumTexCoords; ++Channel) LOD.TexCoords[Channel].resize(VertexCount);
				if (LOD.bHasVertexColors) LOD.Colors.resize(VertexCount);
				LOD.Indices.resize(IndexCounts[LODIndex]);
				LOD.Sections.resize(SectionCounts[LODIndex]);
			}

			FPayloadReader Sections(Chunks[3]);
			for (FStaticMeshPayloadLOD& LOD : Payload.LODs)
			{
				uint32 SectionCount = 0;
				if (!Sections.ReadU32(SectionCount) || SectionCount != LOD.Sections.size())
					return Fail("Static-mesh section chunk count does not match its LOD.", &OutError);
				for (FStaticMeshPayloadSection& Section : LOD.Sections)
				{
					if (!Sections.ReadU32(Section.FirstIndex) || !Sections.ReadU32(Section.IndexCount)
						|| !Sections.ReadU32(Section.MinVertexIndex) || !Sections.ReadU32(Section.MaxVertexIndex)
						|| !Sections.ReadU32(Section.MaterialSlotIndex) || !ReadBounds(Sections, Section.LocalBounds))
						return Fail("Static-mesh section chunk is truncated.", &OutError);
				}
			}
			if (!Sections.IsAtEnd()) return Fail("Static-mesh section chunk contains trailing bytes.", &OutError);

			FPayloadReader VertexStreams(Chunks[4]);
			auto ReadVector = [&VertexStreams]<typename TVector>(TVector& Value, uint32 ComponentCount) -> bool
			{
				for (uint32 Component = 0; Component < ComponentCount; ++Component)
					if (!VertexStreams.ReadFloat(Value[Component]) || !std::isfinite(Value[Component])) return false;
				return true;
			};
			for (FStaticMeshPayloadLOD& LOD : Payload.LODs)
			{
				for (FVector3f& Value : LOD.Positions) if (!ReadVector(Value, 3)) return Fail("Static-mesh positions contain invalid data.", &OutError);
				for (FVector3f& Value : LOD.Normals) if (!ReadVector(Value, 3)) return Fail("Static-mesh normals contain invalid data.", &OutError);
				for (FVector4f& Value : LOD.Tangents) if (!ReadVector(Value, 4)) return Fail("Static-mesh tangents contain invalid data.", &OutError);
				for (uint32 Channel = 0; Channel < LOD.NumTexCoords; ++Channel)
					for (FVector2f& Value : LOD.TexCoords[Channel])
						if (!ReadVector(Value, 2)) return Fail("Static-mesh UVs contain invalid data.", &OutError);
				for (FVector4f& Value : LOD.Colors) if (!ReadVector(Value, 4)) return Fail("Static-mesh colors contain invalid data.", &OutError);
			}
			if (!VertexStreams.IsAtEnd()) return Fail("Static-mesh vertex-stream chunk contains trailing bytes.", &OutError);

			FPayloadReader IndexBuffers(Chunks[5]);
			for (FStaticMeshPayloadLOD& LOD : Payload.LODs)
				for (uint32& Index : LOD.Indices)
					if (!IndexBuffers.ReadU32(Index)) return Fail("Static-mesh index-buffer chunk is truncated.", &OutError);
			if (!IndexBuffers.IsAtEnd()) return Fail("Static-mesh index-buffer chunk contains trailing bytes.", &OutError);

			if (!ValidatePayload(Payload, OutError)) return false;
			OutPayload = std::move(Payload);
			return true;
		}

		using EngineWire::ReadLittleEndianAt;

		auto GetStaticMeshChunkedPayloadFormat() -> Asset::FChunkedPayloadFormat
		{
			static_assert(StaticMeshPayloadHeaderSize == Asset::ChunkedPayloadHeaderSize);
			static_assert(StaticMeshPayloadChunkEntrySize == Asset::ChunkedPayloadEntrySize);
			return {
				.HeaderSizeWordIndex = 5,
				.ChunkCountWordIndex = 6,
				.GlobalFlagsWordIndex = 4,
				.GlobalCompressedFlag = StaticMeshPayloadFlagCompressed,
				.RequiredChunkCount = StaticMeshPayloadRequiredChunkCount,
				.MaximumChunkCount = MaximumStaticMeshPayloadChunks,
				.RequiredChunkFlag = Asset::ChunkedPayloadRequiredFlag,
				.KnownChunkFlags = Asset::ChunkedPayloadRequiredFlag | StaticMeshChunkCompressionMask,
				.CompressionMask = StaticMeshChunkCompressionMask,
				.CompressionShift = 8,
				.MaximumCompressionMethod = StaticMeshChunkCompressionZstandard,
				.MaximumCompressionRatio = StaticMeshMaximumCompressionRatio,
				.MaximumBytes = MaximumStaticMeshPayloadBytes,
				.Alignment = StaticMeshPayloadAlignment,
				.AllowTrailingZeroPadding = true};
		}

		auto FailChunkedPayload(
			const Asset::FChunkedPayloadResult& Result,
			std::string& OutError,
			EPayloadDecodeError* OutCode = nullptr) -> bool
		{
			if (OutCode && Result.Kind == Asset::EChunkedPayloadFailureKind::Incompatible)
				*OutCode = EPayloadDecodeError::Incompatible;
			return Fail(Asset::DescribeChunkedPayloadFailure(Result.Failure, "Static-mesh payload"), &OutError);
		}
	}

	auto BuildStaticMeshSerializedValue(
		const FStaticMeshPayloadData& Payload,
		EStaticMeshTargetPlatform TargetPlatform,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool
	{
		OutError.clear();
		if (TargetPlatform != EStaticMeshTargetPlatform::Win64)
			return Fail("A concrete target platform is required to encode a static-mesh payload.", &OutError);
		if (!ValidatePayload(Payload, OutError)) return false;

		const auto ChunkBytes = BuildPayloadChunks(Payload);
		std::array<Asset::FChunkedPayloadInput, StaticMeshPayloadRequiredChunkCount> Chunks;
		for (uint32 Index = 0; Index < StaticMeshPayloadRequiredChunkCount; ++Index)
			Chunks[Index] = {
				.Type = Index + 1,
				.Flags = Asset::ChunkedPayloadRequiredFlag,
				.Bytes = ChunkBytes[Index],
				.DecodedSize = ChunkBytes[Index].size()};

		const Asset::FChunkedPayloadResult Result = Asset::EncodeChunkedPayload(
			{StaticMeshPayloadMagic, StaticMeshPayloadSchemaVersion, StaticMeshBuilderVersion,
				static_cast<uint32>(TargetPlatform), 0, StaticMeshPayloadHeaderSize,
				StaticMeshPayloadRequiredChunkCount, 0},
			Chunks, GetStaticMeshChunkedPayloadFormat(), OutBytes);
		if (!Result) return FailChunkedPayload(Result, OutError);
		return true;
	}

	auto ParseStaticMeshSerializedValueImpl(
		std::span<const uint8> Bytes,
		EStaticMeshTargetPlatform ExpectedPlatform,
		FStaticMeshPayloadData& OutPayload,
		std::string& OutError,
		EPayloadDecodeError& OutCode) -> bool
	{
		OutError.clear();
		OutCode = EPayloadDecodeError::Corrupt;
		if (Bytes.size() < StaticMeshPayloadHeaderSize) return Fail("Static-mesh payload header is truncated.", &OutError);
		if (ExpectedPlatform != EStaticMeshTargetPlatform::Win64)
		{
			OutCode = EPayloadDecodeError::Incompatible;
			return Fail("A concrete target platform is required to decode a static-mesh payload.", &OutError);
		}

		uint32 Magic = 0;
		uint32 SchemaVersion = 0;
		uint32 BuilderVersion = 0;
		uint32 Platform = 0;
		uint32 PayloadFlags = 0;
		uint32 Reserved = 0;
		if (!ReadLittleEndianAt(Bytes, 0, Magic) || !ReadLittleEndianAt(Bytes, 4, SchemaVersion)
			|| !ReadLittleEndianAt(Bytes, 8, BuilderVersion) || !ReadLittleEndianAt(Bytes, 12, Platform)
			|| !ReadLittleEndianAt(Bytes, 16, PayloadFlags) || !ReadLittleEndianAt(Bytes, 28, Reserved))
			return Fail("Static-mesh payload header is truncated.", &OutError);
		if (Magic != StaticMeshPayloadMagic) return Fail("Static-mesh payload magic is invalid.", &OutError);
		if (SchemaVersion != StaticMeshPayloadSchemaVersion)
		{
			OutCode = EPayloadDecodeError::Incompatible;
			return Fail("Static-mesh payload schema version is unsupported.", &OutError);
		}
		if (BuilderVersion != StaticMeshBuilderVersion)
		{
			OutCode = EPayloadDecodeError::Incompatible;
			return Fail("Static-mesh payload builder version is unsupported.", &OutError);
		}
		if (Platform != static_cast<uint32>(ExpectedPlatform)) return Fail("Static-mesh payload target platform does not match.", &OutError);
		if ((PayloadFlags & ~StaticMeshPayloadFlagCompressed) != 0 || Reserved != 0)
			return Fail("Static-mesh payload header contains invalid flags, sizes, or reserved values.", &OutError);
		Asset::FDecodedChunkedPayload Container;
		const Asset::FChunkedPayloadResult ContainerResult = Asset::DecodeChunkedPayload(
			Bytes, GetStaticMeshChunkedPayloadFormat(), Container);
		if (!ContainerResult) return FailChunkedPayload(ContainerResult, OutError, &OutCode);
		std::array<std::span<const uint8>, StaticMeshPayloadRequiredChunkCount> RequiredChunks;
		for (uint32 Index = 0; Index < StaticMeshPayloadRequiredChunkCount; ++Index)
			RequiredChunks[Index] = Container.RequiredChunks[Index];
		FStaticMeshPayloadData Decoded;
		if (!ReadPayloadChunks(RequiredChunks, Decoded, OutError)) return false;
		OutPayload = std::move(Decoded);
		return true;
	}

	auto ParseStaticMeshSerializedValue(
		std::span<const uint8> Bytes,
		EStaticMeshTargetPlatform ExpectedPlatform,
		FStaticMeshPayloadData& OutPayload) -> FPayloadDecodeResult
	{
		FPayloadDecodeResult Result;
		if (!ParseStaticMeshSerializedValueImpl(
			Bytes, ExpectedPlatform, OutPayload, Result.Message, Result.Code))
			return Result;
		return {};
	}

	auto MakeStaticMeshPayloadData(
		const FStaticMeshRenderData& RenderData,
		FStaticMeshPayloadData& OutPayload,
		std::string& OutError) -> bool
	{
		FStaticMeshPayloadData Payload;
		Payload.LocalBounds = RenderData.LocalBounds;
		Payload.MaterialSlotCount = static_cast<uint32>(RenderData.MaterialSlots.size());
		Payload.LODs.reserve(RenderData.LODResources.size());
		for (const FStaticMeshLODResources& SourceLOD : RenderData.LODResources)
		{
			FStaticMeshPayloadLOD& LOD = Payload.LODs.emplace_back();
			LOD.Positions =
				SourceLOD.VertexBuffers.PositionVertexBuffer
					.GetPositions();
			LOD.Normals =
				SourceLOD.VertexBuffers.StaticMeshVertexBuffer
					.TangentsVertexBuffer.GetNormals();
			LOD.Tangents =
				SourceLOD.VertexBuffers.StaticMeshVertexBuffer
					.TangentsVertexBuffer.GetTangents();
			LOD.Indices = SourceLOD.IndexBuffer.GetIndices();
			LOD.LocalBounds = SourceLOD.LocalBounds;
			LOD.ScreenSize = SourceLOD.ScreenSize;
			LOD.NumTexCoords = SourceLOD.NumTexCoords;
			LOD.bHasVertexColors =
				SourceLOD.bHasColorVertexData;
			const auto& SourceTexCoords =
				SourceLOD.VertexBuffers.StaticMeshVertexBuffer
					.TexCoordVertexBuffer.GetTexCoords();
			for (uint32 Channel = 0; Channel < LOD.NumTexCoords; ++Channel)
			{
				LOD.TexCoords[Channel] = SourceTexCoords[Channel];
			}
			if (LOD.bHasVertexColors)
			{
				LOD.Colors =
					SourceLOD.VertexBuffers.ColorVertexBuffer
						.GetColors();
			}
			LOD.Sections.reserve(SourceLOD.Sections.size());
			for (const FStaticMeshSection& SourceSection : SourceLOD.Sections)
			{
				LOD.Sections.push_back({
					.FirstIndex = SourceSection.FirstIndex,
					.IndexCount = SourceSection.IndexCount,
					.MinVertexIndex = SourceSection.MinVertexIndex,
					.MaxVertexIndex = SourceSection.MaxVertexIndex,
					.MaterialSlotIndex = SourceSection.MaterialSlotIndex,
					.LocalBounds = SourceSection.LocalBounds});
			}
		}
		if (!ValidatePayload(Payload, OutError)) return false;
		OutPayload = std::move(Payload);
		return true;
	}

	auto MakeStaticMeshRenderData(
		const FStaticMeshPayloadData& Payload,
		std::unique_ptr<FStaticMeshRenderData>& OutRenderData,
		std::string& OutError) -> bool
	{
		if (!ValidatePayload(Payload, OutError)) return false;
		auto RenderData = std::make_unique<FStaticMeshRenderData>();
		RenderData->LocalBounds = Payload.LocalBounds;
		RenderData->MaterialSlots.resize(Payload.MaterialSlotCount);
		RenderData->LODResources.reserve(Payload.LODs.size());
		for (const FStaticMeshPayloadLOD& SourceLOD : Payload.LODs)
		{
			FStaticMeshLODResources& LOD = RenderData->LODResources.emplace_back();
			LOD.VertexBuffers.PositionVertexBuffer.Init(
				SourceLOD.Positions);
			LOD.VertexBuffers.ColorVertexBuffer.Init(
				SourceLOD.Colors,
				static_cast<uint32>(
					SourceLOD.Positions.size()));
			LOD.VertexBuffers.StaticMeshVertexBuffer
				.TangentsVertexBuffer.Init(
					SourceLOD.Normals,
					SourceLOD.Tangents);
			LOD.VertexBuffers.StaticMeshVertexBuffer
				.TexCoordVertexBuffer.Init(
					SourceLOD.TexCoords,
					static_cast<uint32>(
						SourceLOD.Positions.size()),
					SourceLOD.NumTexCoords);
			LOD.IndexBuffer.Init(SourceLOD.Indices);
			LOD.LocalBounds = SourceLOD.LocalBounds;
			LOD.ScreenSize = SourceLOD.ScreenSize;
			LOD.NumTexCoords = SourceLOD.NumTexCoords;
			LOD.bHasColorVertexData =
				SourceLOD.bHasVertexColors;
			LOD.VertexBuffers.Finalize(
				LOD.NumTexCoords,
				LOD.bHasColorVertexData);
			LOD.Sections.reserve(SourceLOD.Sections.size());
			for (const FStaticMeshPayloadSection& SourceSection : SourceLOD.Sections)
			{
				LOD.Sections.push_back({
					.FirstIndex = SourceSection.FirstIndex,
					.IndexCount = SourceSection.IndexCount,
					.MinVertexIndex = SourceSection.MinVertexIndex,
					.MaxVertexIndex = SourceSection.MaxVertexIndex,
					.MaterialSlotIndex = SourceSection.MaterialSlotIndex,
					.LocalBounds = SourceSection.LocalBounds});
			}
		}
		OutRenderData = std::move(RenderData);
		return true;
	}

	namespace
	{
		auto WriteCollisionU32(std::vector<uint8>& Bytes, size_t Offset, uint32 Value) -> void
		{
			for (uint32 Byte = 0; Byte < 4; ++Byte)
				Bytes[Offset + Byte] = static_cast<uint8>(Value >> (Byte * 8));
		}

		auto WriteCollisionU64(std::vector<uint8>& Bytes, size_t Offset, uint64 Value) -> void
		{
			for (uint32 Byte = 0; Byte < 8; ++Byte)
				Bytes[Offset + Byte] = static_cast<uint8>(Value >> (Byte * 8));
		}

		auto AlignCollisionOffset(uint64 Offset) -> uint64
		{
			return (Offset + StaticMeshCollisionPayloadAlignment - 1)
				& ~(static_cast<uint64>(StaticMeshCollisionPayloadAlignment) - 1);
		}

		auto CollisionDecodeFailure(EPayloadDecodeError Code, std::string Message)
			-> FPayloadDecodeResult
		{
			return {Code, std::move(Message)};
		}
	}

	auto MakeStaticMeshCollisionPayloadData(
		const FCollisionGeometryRef& Geometry,
		EBodySetupCollisionQueryPolicy QueryPolicy,
		FStaticMeshCollisionPayloadData& OutPayload,
		std::string& OutError) -> bool
	{
		if (!Geometry || (Geometry.GetKind() != ECollisionGeometryKind::ConvexHull
			&& Geometry.GetKind() != ECollisionGeometryKind::TriangleMesh))
			return Fail("Collision payload requires one valid hull or triangle mesh.", &OutError);
		FStaticMeshCollisionPayloadData Candidate;
		Candidate.SourceMode = Geometry.GetKind() == ECollisionGeometryKind::ConvexHull
			? EBodySetupCollisionSourceMode::ConvexHullFromLOD0
			: EBodySetupCollisionSourceMode::TriangleMeshFromLOD0;
		Candidate.QueryPolicy = QueryPolicy;
		Candidate.Positions.reserve(Geometry.GetVertexCount());
		for (uint32 Index = 0; Index < Geometry.GetVertexCount(); ++Index)
		{
			const FVector3* Vertex = Geometry.GetVertex(Index);
			if (!Vertex || !Math::IsFinite(*Vertex))
				return Fail("Collision geometry contains an invalid vertex.", &OutError);
			const FVector3f Stored(*Vertex);
			if (!Math::IsFinite(Stored))
				return Fail("Collision vertex is outside finite float32 storage.", &OutError);
			Candidate.Positions.push_back(Stored);
		}
		Candidate.Indices.reserve(Geometry.GetTriangleCount() * 3);
		Candidate.SourceOrdinals.reserve(Geometry.GetTriangleCount());
		for (uint32 Index = 0; Index < Geometry.GetTriangleCount(); ++Index)
		{
			const FCollisionGeometryTriangle* Triangle = Geometry.GetTriangle(Index);
			if (!Triangle) return Fail("Collision geometry has an invalid triangle.", &OutError);
			Candidate.Indices.insert(Candidate.Indices.end(),
				{Triangle->First, Triangle->Second, Triangle->Third});
			Candidate.SourceOrdinals.push_back(Triangle->SourceOrdinal);
		}
		Candidate.Nodes.reserve(Geometry.GetNodeCount());
		for (uint32 Index = 0; Index < Geometry.GetNodeCount(); ++Index)
		{
			const FCollisionGeometryNode* Node = Geometry.GetNode(Index);
			if (!Node) return Fail("Collision geometry has an invalid BVH node.", &OutError);
			Candidate.Nodes.push_back(*Node);
		}
		Candidate.LeafTriangles.reserve(Geometry.GetLeafTriangleCount());
		for (uint32 Index = 0; Index < Geometry.GetLeafTriangleCount(); ++Index)
		{
			const uint32 TriangleIndex = Geometry.GetLeafTriangle(Index);
			const FCollisionGeometryTriangle* Triangle = Geometry.GetTriangle(TriangleIndex);
			if (!Triangle) return Fail("Collision geometry has an invalid BVH membership.", &OutError);
			Candidate.LeafTriangles.push_back(Triangle->SourceOrdinal);
		}
		OutPayload = std::move(Candidate);
		OutError.clear();
		return true;
	}

	auto MakeStaticMeshCollisionGeometry(
		const FStaticMeshCollisionPayloadData& Payload,
		FCollisionGeometryRef& OutGeometry,
		std::string& OutError) -> bool
	{
		if (Payload.Positions.empty() || Payload.Indices.empty()
			|| Payload.Indices.size() % 3 != 0
			|| Payload.SourceOrdinals.size() != Payload.Indices.size() / 3)
			return Fail("Collision payload counts are inconsistent.", &OutError);
		std::vector<FVector3> Vertices;
		Vertices.reserve(Payload.Positions.size());
		for (const FVector3f& Position : Payload.Positions)
		{
			if (!Math::IsFinite(Position)) return Fail("Collision payload contains a non-finite position.", &OutError);
			Vertices.emplace_back(Position);
		}
		FCollisionGeometryRef Candidate;
		if (Payload.SourceMode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0)
		{
			if (!Payload.Nodes.empty() || !Payload.LeafTriangles.empty())
				return Fail("Convex collision payload must not contain a BVH.", &OutError);
			Candidate = FCollisionGeometryRef::MakeConvexHull(Vertices, Payload.Indices);
		}
		else if (Payload.SourceMode == EBodySetupCollisionSourceMode::TriangleMeshFromLOD0)
		{
			std::map<uint32, uint32> OrdinalToTriangle;
			for (uint32 Triangle = 0; Triangle < Payload.SourceOrdinals.size(); ++Triangle)
				if (!OrdinalToTriangle.emplace(Payload.SourceOrdinals[Triangle], Triangle).second)
					return Fail("Collision payload source ordinals are not unique.", &OutError);
			std::vector<uint32> LeafTriangles;
			LeafTriangles.reserve(Payload.LeafTriangles.size());
			for (uint32 Ordinal : Payload.LeafTriangles)
			{
				const auto Found = OrdinalToTriangle.find(Ordinal);
				if (Found == OrdinalToTriangle.end())
					return Fail("Collision BVH references an unknown source ordinal.", &OutError);
				LeafTriangles.push_back(Found->second);
			}
			Candidate = FCollisionGeometryRef::MakeCookedTriangleMesh(
				Vertices, Payload.Indices, Payload.SourceOrdinals, Payload.Nodes, LeafTriangles);
		}
		else return Fail("Collision payload source mode is invalid.", &OutError);
		if (!Candidate) return Fail("Collision payload topology or BVH is invalid.", &OutError);
		OutGeometry = Candidate;
		OutError.clear();
		return true;
	}

	auto BuildStaticMeshCollisionSerializedValue(
		const FStaticMeshCollisionPayloadData& Payload,
		EStaticMeshTargetPlatform TargetPlatform,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool
	{
		if (TargetPlatform != EStaticMeshTargetPlatform::Win64)
			return Fail("A concrete target platform is required for DCOL encoding.", &OutError);
		FCollisionGeometryRef ValidationGeometry;
		if (!MakeStaticMeshCollisionGeometry(Payload, ValidationGeometry, OutError)) return false;
		std::vector<uint32> StoredIndices;
		std::vector<uint32> StoredOrdinals;
		if (Payload.SourceMode == EBodySetupCollisionSourceMode::TriangleMeshFromLOD0)
		{
			std::map<uint32, uint32> OrdinalToTriangle;
			for (uint32 Triangle = 0; Triangle < Payload.SourceOrdinals.size(); ++Triangle)
				OrdinalToTriangle.emplace(Payload.SourceOrdinals[Triangle], Triangle);
			StoredIndices.reserve(Payload.Indices.size());
			StoredOrdinals.reserve(Payload.LeafTriangles.size());
			for (uint32 Ordinal : Payload.LeafTriangles)
			{
				const auto Found = OrdinalToTriangle.find(Ordinal);
				if (Found == OrdinalToTriangle.end())
					return Fail("DCOL leaf ordering references an unknown source ordinal.", &OutError);
				const uint32 Triangle = Found->second;
				StoredIndices.insert(StoredIndices.end(), Payload.Indices.begin() + Triangle * 3,
					Payload.Indices.begin() + Triangle * 3 + 3);
				StoredOrdinals.push_back(Ordinal);
			}
		}
		else
		{
			StoredIndices = Payload.Indices;
			StoredOrdinals = Payload.SourceOrdinals;
		}
		std::array<std::vector<uint8>, 4> Chunks;
		FPayloadWriter Positions;
		for (const FVector3f& Position : Payload.Positions)
			for (uint32 Axis = 0; Axis < 3; ++Axis) Positions.WriteFloat(Position[Axis]);
		Chunks[0] = Positions.TakeBytes();
		FPayloadWriter Indices;
		for (uint32 Index : StoredIndices) Indices.WriteU32(Index);
		Chunks[1] = Indices.TakeBytes();
		FPayloadWriter Ordinals;
		for (uint32 Ordinal : StoredOrdinals) Ordinals.WriteU32(Ordinal);
		Chunks[2] = Ordinals.TakeBytes();
		FPayloadWriter Nodes;
		for (const FCollisionGeometryNode& Node : Payload.Nodes)
		{
			for (uint32 Axis = 0; Axis < 3; ++Axis) Nodes.WriteFloat(Node.Minimum[Axis]);
			Nodes.WriteU32(Node.First);
			for (uint32 Axis = 0; Axis < 3; ++Axis) Nodes.WriteFloat(Node.Maximum[Axis]);
			Nodes.WriteU32(Node.CountOrSecond);
		}
		Chunks[3] = Nodes.TakeBytes();
		const std::array<uint64, 4> Counts{Payload.Positions.size(), StoredIndices.size(),
			StoredOrdinals.size(), Payload.Nodes.size()};
		std::vector<uint8> Bytes(StaticMeshCollisionPayloadHeaderSize
			+ Chunks.size() * StaticMeshCollisionPayloadChunkEntrySize, 0);
		for (uint32 Chunk = 0; Chunk < Chunks.size(); ++Chunk)
		{
			const uint64 Offset = AlignCollisionOffset(Bytes.size());
			if (Offset > MaximumStaticMeshCollisionPayloadBytes
				|| Chunks[Chunk].size() > MaximumStaticMeshCollisionPayloadBytes - Offset)
				return Fail("DCOL payload exceeds the runtime byte limit.", &OutError);
			Bytes.resize(static_cast<size_t>(Offset), 0);
			const size_t Entry = StaticMeshCollisionPayloadHeaderSize
				+ Chunk * StaticMeshCollisionPayloadChunkEntrySize;
			WriteCollisionU32(Bytes, Entry, Chunk + 1);
			WriteCollisionU32(Bytes, Entry + 4, 1);
			WriteCollisionU64(Bytes, Entry + 8, Offset);
			WriteCollisionU64(Bytes, Entry + 16, Chunks[Chunk].size());
			WriteCollisionU64(Bytes, Entry + 24, Counts[Chunk]);
			Bytes.insert(Bytes.end(), Chunks[Chunk].begin(), Chunks[Chunk].end());
		}
		const uint64 LogicalBytes = Payload.Positions.size() * sizeof(FVector3f)
			+ Payload.Indices.size() * sizeof(uint32)
			+ Payload.SourceOrdinals.size() * sizeof(uint32)
			+ Payload.Nodes.size() * sizeof(FCollisionGeometryNode);
		WriteCollisionU32(Bytes, 0, StaticMeshCollisionPayloadMagic);
		WriteCollisionU32(Bytes, 4, StaticMeshCollisionPayloadSchemaVersion);
		WriteCollisionU32(Bytes, 8, StaticMeshCollisionBuilderVersion);
		WriteCollisionU32(Bytes, 12, static_cast<uint32>(TargetPlatform));
		WriteCollisionU32(Bytes, 16, StaticMeshCollisionPayloadHeaderSize);
		WriteCollisionU32(Bytes, 20, static_cast<uint32>(Chunks.size()));
		WriteCollisionU32(Bytes, 24, StaticMeshCollisionPayloadAlignment);
		WriteCollisionU32(Bytes, 28, 0);
		WriteCollisionU64(Bytes, 32, Bytes.size());
		WriteCollisionU64(Bytes, 40, LogicalBytes);
		WriteCollisionU64(Bytes, 48, FXxHash64::HashBuffer(
			std::span<const uint8>(Bytes).subspan(64)).HashValue);
		WriteCollisionU32(Bytes, 56, 0);
		WriteCollisionU32(Bytes, 60, 0);
		OutBytes = std::move(Bytes);
		OutError.clear();
		return true;
	}

	auto ParseStaticMeshCollisionSerializedValue(
		std::span<const uint8> Bytes,
		EStaticMeshTargetPlatform ExpectedPlatform,
		FStaticMeshCollisionPayloadData& OutPayload) -> FPayloadDecodeResult
	{
		uint32 Magic = 0, Schema = 0, Builder = 0, Platform = 0, Header = 0;
		uint32 ChunkCount = 0, Alignment = 0, Mode = 0, Policy = 0, Reserved = 0;
		uint64 StoredSize = 0, LogicalBytes = 0, Checksum = 0;
		if (Bytes.size() < StaticMeshCollisionPayloadHeaderSize
			|| !ReadLittleEndianAt(Bytes, 0, Magic) || !ReadLittleEndianAt(Bytes, 4, Schema)
			|| !ReadLittleEndianAt(Bytes, 8, Builder) || !ReadLittleEndianAt(Bytes, 12, Platform)
			|| !ReadLittleEndianAt(Bytes, 16, Header) || !ReadLittleEndianAt(Bytes, 20, ChunkCount)
			|| !ReadLittleEndianAt(Bytes, 24, Alignment) || !ReadLittleEndianAt(Bytes, 28, Mode)
			|| !ReadLittleEndianAt(Bytes, 32, StoredSize) || !ReadLittleEndianAt(Bytes, 40, LogicalBytes)
			|| !ReadLittleEndianAt(Bytes, 48, Checksum) || !ReadLittleEndianAt(Bytes, 56, Policy)
			|| !ReadLittleEndianAt(Bytes, 60, Reserved))
			return CollisionDecodeFailure(EPayloadDecodeError::Corrupt, "DCOL header is truncated.");
		if (Magic != StaticMeshCollisionPayloadMagic || Schema != StaticMeshCollisionPayloadSchemaVersion
			|| Builder != StaticMeshCollisionBuilderVersion
			|| Platform != static_cast<uint32>(ExpectedPlatform))
			return CollisionDecodeFailure(EPayloadDecodeError::Incompatible, "DCOL identity, version, or platform is incompatible.");
		if (Header != StaticMeshCollisionPayloadHeaderSize || ChunkCount != 4
			|| ChunkCount > MaximumStaticMeshCollisionPayloadChunks
			|| Alignment != StaticMeshCollisionPayloadAlignment || Mode != 0 || Policy != 0 || Reserved != 0
			|| StoredSize != Bytes.size() || StoredSize > MaximumStaticMeshCollisionPayloadBytes
			|| Checksum != FXxHash64::HashBuffer(Bytes.subspan(64)).HashValue)
			return CollisionDecodeFailure(EPayloadDecodeError::Corrupt, "DCOL header values or checksum are invalid.");
		const std::array<uint64, 4> ElementSizes{12, 4, 4, 32};
		std::array<std::span<const uint8>, 4> Chunks;
		std::array<uint64, 4> Counts{};
		uint64 PreviousEnd = StaticMeshCollisionPayloadHeaderSize
			+ ChunkCount * StaticMeshCollisionPayloadChunkEntrySize;
		for (uint32 Chunk = 0; Chunk < ChunkCount; ++Chunk)
		{
			const size_t Entry = StaticMeshCollisionPayloadHeaderSize
				+ Chunk * StaticMeshCollisionPayloadChunkEntrySize;
			uint32 Type = 0, Flags = 0;
			uint64 Offset = 0, Size = 0, Count = 0;
			if (!ReadLittleEndianAt(Bytes, Entry, Type) || !ReadLittleEndianAt(Bytes, Entry + 4, Flags)
				|| !ReadLittleEndianAt(Bytes, Entry + 8, Offset) || !ReadLittleEndianAt(Bytes, Entry + 16, Size)
				|| !ReadLittleEndianAt(Bytes, Entry + 24, Count) || Type != Chunk + 1 || Flags != 1
				|| Offset % Alignment != 0 || Offset < PreviousEnd || Offset > Bytes.size()
				|| Size > Bytes.size() - Offset || Count > std::numeric_limits<uint64>::max() / ElementSizes[Chunk]
				|| Count * ElementSizes[Chunk] != Size)
				return CollisionDecodeFailure(EPayloadDecodeError::Corrupt, "DCOL chunk table is invalid.");
			for (uint64 Padding = PreviousEnd; Padding < Offset; ++Padding)
				if (Bytes[Padding] != 0)
					return CollisionDecodeFailure(EPayloadDecodeError::Corrupt, "DCOL alignment padding is non-zero.");
			Chunks[Chunk] = Bytes.subspan(static_cast<size_t>(Offset), static_cast<size_t>(Size));
			Counts[Chunk] = Count;
			PreviousEnd = Offset + Size;
		}
		if (PreviousEnd != Bytes.size() || Counts[0] == 0 || Counts[1] == 0
			|| Counts[1] % 3 != 0 || Counts[2] != Counts[1] / 3
			|| Counts[0] > MaximumStaticMeshVerticesPerLOD
			|| Counts[2] > 2'000'000 || LogicalBytes != Counts[0] * 12 + Counts[1] * 4
				+ Counts[2] * 4 + Counts[3] * 32)
			return CollisionDecodeFailure(EPayloadDecodeError::Corrupt, "DCOL counts or logical byte total are invalid.");
		FStaticMeshCollisionPayloadData Candidate;
		Candidate.SourceMode = Counts[3] == 0
			? EBodySetupCollisionSourceMode::ConvexHullFromLOD0
			: EBodySetupCollisionSourceMode::TriangleMeshFromLOD0;
		Candidate.QueryPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		FPayloadReader PositionReader(Chunks[0]);
		Candidate.Positions.resize(static_cast<size_t>(Counts[0]));
		for (FVector3f& Position : Candidate.Positions)
			for (uint32 Axis = 0; Axis < 3; ++Axis)
				if (!PositionReader.ReadFloat(Position[Axis]) || !std::isfinite(Position[Axis]))
					return CollisionDecodeFailure(EPayloadDecodeError::Corrupt, "DCOL position data is invalid.");
		auto ReadU32Chunk = [](std::span<const uint8> Bytes, uint64 Count, std::vector<uint32>& Out) {
			FPayloadReader Reader(Bytes);
			Out.resize(static_cast<size_t>(Count));
			for (uint32& Value : Out) if (!Reader.ReadU32(Value)) return false;
			return Reader.IsAtEnd();
		};
		if (!ReadU32Chunk(Chunks[1], Counts[1], Candidate.Indices)
			|| !ReadU32Chunk(Chunks[2], Counts[2], Candidate.SourceOrdinals))
			return CollisionDecodeFailure(EPayloadDecodeError::Corrupt, "DCOL index or ordinal data is invalid.");
		FPayloadReader NodeReader(Chunks[3]);
		Candidate.Nodes.resize(static_cast<size_t>(Counts[3]));
		for (FCollisionGeometryNode& Node : Candidate.Nodes)
		{
			for (uint32 Axis = 0; Axis < 3; ++Axis) if (!NodeReader.ReadFloat(Node.Minimum[Axis]))
				return CollisionDecodeFailure(EPayloadDecodeError::Corrupt, "DCOL node data is truncated.");
			if (!NodeReader.ReadU32(Node.First))
				return CollisionDecodeFailure(EPayloadDecodeError::Corrupt, "DCOL node data is truncated.");
			for (uint32 Axis = 0; Axis < 3; ++Axis) if (!NodeReader.ReadFloat(Node.Maximum[Axis]))
				return CollisionDecodeFailure(EPayloadDecodeError::Corrupt, "DCOL node data is truncated.");
			if (!NodeReader.ReadU32(Node.CountOrSecond))
				return CollisionDecodeFailure(EPayloadDecodeError::Corrupt, "DCOL node data is truncated.");
		}
		if (Candidate.SourceMode == EBodySetupCollisionSourceMode::TriangleMeshFromLOD0)
			Candidate.LeafTriangles = Candidate.SourceOrdinals;
		FCollisionGeometryRef Geometry;
		std::string Error;
		if (!MakeStaticMeshCollisionGeometry(Candidate, Geometry, Error))
			return CollisionDecodeFailure(EPayloadDecodeError::Corrupt, std::move(Error));
		OutPayload = std::move(Candidate);
		return {};
	}

	auto FStaticMeshPayloadData::Serialize(
		FArchive& Ar,
		EStaticMeshTargetPlatform TargetPlatform) -> void
	{
		SerializeBoundedArchivePayload<FStaticMeshPayloadData>(
			Ar,
			{MaximumStaticMeshPayloadBytes, "Static-mesh payload"},
			[&](std::vector<uint8>& Bytes, std::string& Error) {
				return BuildStaticMeshSerializedValue(
					*this, TargetPlatform, Bytes, Error);
			},
			[&](std::span<const uint8> Bytes, FStaticMeshPayloadData& Candidate) {
				return ParseStaticMeshSerializedValue(Bytes, TargetPlatform, Candidate);
			},
			[&](FStaticMeshPayloadData&& Candidate) {
				*this = std::move(Candidate);
			});
	}

	auto FStaticMeshCollisionPayloadData::Serialize(
		FArchive& Ar,
		EStaticMeshTargetPlatform TargetPlatform) -> void
	{
		SerializeBoundedArchivePayload<FStaticMeshCollisionPayloadData>(
			Ar,
			{MaximumStaticMeshCollisionPayloadBytes, "DCOL payload"},
			[&](std::vector<uint8>& Bytes, std::string& Error) {
				return BuildStaticMeshCollisionSerializedValue(
					*this, TargetPlatform, Bytes, Error);
			},
			[&](std::span<const uint8> Bytes,
				FStaticMeshCollisionPayloadData& Candidate) {
				return ParseStaticMeshCollisionSerializedValue(
					Bytes, TargetPlatform, Candidate);
			},
			[&](FStaticMeshCollisionPayloadData&& Candidate) {
				*this = std::move(Candidate);
			});
	}
}
