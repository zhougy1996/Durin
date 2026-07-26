#include "StaticMesh/StaticMeshDerivedData.h"

#include "Misc/DerivedDataCache.h"

namespace Durin
{
	namespace
	{
		inline constexpr uint32 StaticMeshPayloadRequiredChunkCount = 6;
		inline constexpr uint32 StaticMeshPayloadFlagCompressed = 1;
		inline constexpr uint32 StaticMeshChunkFlagRequired = 1;
		inline constexpr uint32 StaticMeshChunkCompressionMask = 0x0000ff00;
		inline constexpr uint32 StaticMeshChunkKnownFlags = StaticMeshChunkFlagRequired | StaticMeshChunkCompressionMask;
		inline constexpr uint32 StaticMeshChunkCompressionNone = 0;
		inline constexpr uint32 StaticMeshChunkCompressionZstandard = 1;
		inline constexpr uint64 StaticMeshMaximumCompressionRatio = 64;

		struct FStaticMeshPayloadChunk
		{
			uint32 Type = 0;
			uint32 Flags = 0;
			uint64 Offset = 0;
			uint64 StoredSize = 0;
			uint64 UncompressedSize = 0;
		};

		class FPayloadWriter
		{
		public:
			auto WriteU8(uint8 Value) -> void { Bytes.push_back(Value); }

			auto WriteU16(uint16 Value) -> void
			{
				WriteU8(static_cast<uint8>(Value));
				WriteU8(static_cast<uint8>(Value >> 8));
			}

			auto WriteU32(uint32 Value) -> void
			{
				for (uint32 Byte = 0; Byte < 4; ++Byte) WriteU8(static_cast<uint8>(Value >> (Byte * 8)));
			}

			auto WriteU64(uint64 Value) -> void
			{
				for (uint32 Byte = 0; Byte < 8; ++Byte) WriteU8(static_cast<uint8>(Value >> (Byte * 8)));
			}

			auto WriteFloat(float Value) -> void { WriteU32(std::bit_cast<uint32>(Value)); }
			auto WriteBytes(std::span<const uint8> Value) -> void { Bytes.insert(Bytes.end(), Value.begin(), Value.end()); }
			auto WriteZeroes(size_t Count) -> void { Bytes.insert(Bytes.end(), Count, 0); }
			auto GetBytes() const -> const std::vector<uint8>& { return Bytes; }
			auto TakeBytes() -> std::vector<uint8> { return std::move(Bytes); }

		private:
			std::vector<uint8> Bytes;
		};

		class FPayloadReader
		{
		public:
			explicit FPayloadReader(std::span<const uint8> InBytes) : Bytes(InBytes) {}

			auto ReadU8(uint8& Value) -> bool
			{
				if (Offset >= Bytes.size()) return false;
				Value = Bytes[Offset++];
				return true;
			}

			auto ReadU16(uint16& Value) -> bool
			{
				uint8 Low = 0;
				uint8 High = 0;
				if (!ReadU8(Low) || !ReadU8(High)) return false;
				Value = static_cast<uint16>(Low | (static_cast<uint16>(High) << 8));
				return true;
			}

			auto ReadU32(uint32& Value) -> bool
			{
				if (GetRemainingBytes() < 4) return false;
				Value = 0;
				for (uint32 Byte = 0; Byte < 4; ++Byte) Value |= static_cast<uint32>(Bytes[Offset++]) << (Byte * 8);
				return true;
			}

			auto ReadU64(uint64& Value) -> bool
			{
				if (GetRemainingBytes() < 8) return false;
				Value = 0;
				for (uint32 Byte = 0; Byte < 8; ++Byte) Value |= static_cast<uint64>(Bytes[Offset++]) << (Byte * 8);
				return true;
			}

			auto ReadFloat(float& Value) -> bool
			{
				uint32 Bits = 0;
				if (!ReadU32(Bits)) return false;
				Value = std::bit_cast<float>(Bits);
				return true;
			}

			auto IsAtEnd() const -> bool { return Offset == Bytes.size(); }
			auto GetRemainingBytes() const -> size_t { return Bytes.size() - Offset; }

		private:
			std::span<const uint8> Bytes;
			size_t Offset = 0;
		};

		auto Fail(std::string& OutError, std::string Message) -> bool
		{
			OutError = std::move(Message);
			return false;
		}

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
			if (!IsValidBounds(Payload.LocalBounds)) return Fail(OutError, "Static-mesh payload bounds are invalid or not exactly representable as float32.");
			if (Payload.MaterialSlotIds.empty() || Payload.MaterialSlotIds.size() > MaximumStaticMeshMaterialSlots)
				return Fail(OutError, "Static-mesh payload material-slot count is outside the supported range.");
			if (std::ranges::any_of(Payload.MaterialSlotIds, [](const FGuid& SlotId) { return !SlotId.IsValid(); }))
				return Fail(OutError, "Static-mesh payload contains an invalid material-slot identifier.");
			if (Payload.LODs.empty() || Payload.LODs.size() > MaximumStaticMeshLODs)
				return Fail(OutError, "Static-mesh payload LOD count is outside the supported range.");

			uint64 EncodedSizeUpperBound = StaticMeshPayloadHeaderSize
				+ StaticMeshPayloadRequiredChunkCount * StaticMeshPayloadChunkEntrySize
				+ StaticMeshPayloadRequiredChunkCount * (StaticMeshPayloadAlignment - 1)
				+ 24ull
				+ 4ull + static_cast<uint64>(Payload.MaterialSlotIds.size()) * 16ull
				+ 4ull + static_cast<uint64>(Payload.LODs.size()) * 40ull;
			for (size_t LODIndex = 0; LODIndex < Payload.LODs.size(); ++LODIndex)
			{
				const FStaticMeshPayloadLOD& LOD = Payload.LODs[LODIndex];
				const size_t VertexCount = LOD.Positions.size();
				const size_t IndexCount = LOD.Indices.size();
				if (VertexCount == 0 || VertexCount > MaximumStaticMeshVerticesPerLOD)
					return Fail(OutError, std::format("Static-mesh payload LOD {} has an invalid vertex count.", LODIndex));
				if (IndexCount == 0 || IndexCount > MaximumStaticMeshIndicesPerLOD)
					return Fail(OutError, std::format("Static-mesh payload LOD {} has an invalid index count.", LODIndex));
				if (LOD.Sections.empty() || LOD.Sections.size() > MaximumStaticMeshSectionsPerLOD)
					return Fail(OutError, std::format("Static-mesh payload LOD {} has an invalid section count.", LODIndex));
				if (LOD.NumTexCoords > MaxStaticMeshUVChannels)
					return Fail(OutError, std::format("Static-mesh payload LOD {} has an invalid UV-channel count.", LODIndex));
				const uint64 LODPayloadBytes = 4ull + static_cast<uint64>(LOD.Sections.size()) * 44ull
					+ static_cast<uint64>(VertexCount) * (40ull + static_cast<uint64>(LOD.NumTexCoords) * 8ull
						+ (LOD.bHasVertexColors ? 16ull : 0ull))
					+ static_cast<uint64>(IndexCount) * 4ull;
				if (LODPayloadBytes > MaximumStaticMeshPayloadBytes - EncodedSizeUpperBound)
					return Fail(OutError, "Static-mesh payload exceeds the stored-object size limit.");
				EncodedSizeUpperBound += LODPayloadBytes;
				if (!IsValidBounds(LOD.LocalBounds))
					return Fail(OutError, std::format("Static-mesh payload LOD {} bounds are invalid.", LODIndex));
				if (LOD.Normals.size() != VertexCount || LOD.Tangents.size() != VertexCount)
					return Fail(OutError, std::format("Static-mesh payload LOD {} vertex-stream counts do not match.", LODIndex));
				for (uint32 Channel = 0; Channel < MaxStaticMeshUVChannels; ++Channel)
				{
					const size_t ExpectedCount = Channel < LOD.NumTexCoords ? VertexCount : 0;
					if (LOD.TexCoords[Channel].size() != ExpectedCount)
						return Fail(OutError, std::format(
							"Static-mesh payload LOD {} UV stream {} has {} values; expected {}.",
							LODIndex, Channel, LOD.TexCoords[Channel].size(), ExpectedCount));
				}
				if (LOD.Colors.size() != (LOD.bHasVertexColors ? VertexCount : 0))
					return Fail(OutError, std::format("Static-mesh payload LOD {} color-stream count does not match its flags.", LODIndex));
				if (std::ranges::any_of(LOD.Positions, [](const FVector3f& Value) { return !IsFinite(Value); })
					|| std::ranges::any_of(LOD.Normals, [](const FVector3f& Value) { return !IsFinite(Value); })
					|| std::ranges::any_of(LOD.Tangents, [](const FVector4f& Value) { return !IsFinite(Value); })
					|| std::ranges::any_of(LOD.Colors, [](const FVector4f& Value) { return !IsFinite(Value); }))
					return Fail(OutError, std::format("Static-mesh payload LOD {} contains a non-finite vertex attribute.", LODIndex));
				for (uint32 Channel = 0; Channel < LOD.NumTexCoords; ++Channel)
				{
					if (std::ranges::any_of(LOD.TexCoords[Channel], [](const FVector2f& Value) { return !IsFinite(Value); }))
						return Fail(OutError, std::format("Static-mesh payload LOD {} contains a non-finite UV.", LODIndex));
				}
				if (std::ranges::any_of(LOD.Indices, [VertexCount](uint32 Index) { return Index >= VertexCount; }))
					return Fail(OutError, std::format("Static-mesh payload LOD {} contains an out-of-range index.", LODIndex));

				uint64 CoveredIndices = 0;
				for (const FStaticMeshPayloadSection& Section : LOD.Sections)
				{
					const uint64 SectionEnd = static_cast<uint64>(Section.FirstIndex) + Section.IndexCount;
					if (Section.IndexCount == 0 || Section.FirstIndex != CoveredIndices || SectionEnd > IndexCount)
						return Fail(OutError, std::format("Static-mesh payload LOD {} sections do not exactly cover its index buffer.", LODIndex));
					if (Section.MinVertexIndex > Section.MaxVertexIndex || Section.MaxVertexIndex >= VertexCount)
						return Fail(OutError, std::format("Static-mesh payload LOD {} section has an invalid vertex range.", LODIndex));
					if (Section.MaterialSlotIndex >= Payload.MaterialSlotIds.size())
						return Fail(OutError, std::format("Static-mesh payload LOD {} section has an invalid material slot.", LODIndex));
					if (!IsValidBounds(Section.LocalBounds))
						return Fail(OutError, std::format("Static-mesh payload LOD {} section bounds are invalid.", LODIndex));

					uint32 ActualMinimum = std::numeric_limits<uint32>::max();
					uint32 ActualMaximum = 0;
					for (uint64 IndexOffset = Section.FirstIndex; IndexOffset < SectionEnd; ++IndexOffset)
					{
						ActualMinimum = std::min(ActualMinimum, LOD.Indices[static_cast<size_t>(IndexOffset)]);
						ActualMaximum = std::max(ActualMaximum, LOD.Indices[static_cast<size_t>(IndexOffset)]);
					}
					if (ActualMinimum != Section.MinVertexIndex || ActualMaximum != Section.MaxVertexIndex)
						return Fail(OutError, std::format("Static-mesh payload LOD {} section vertex range does not match its indices.", LODIndex));
					CoveredIndices = SectionEnd;
				}
				if (CoveredIndices != IndexCount)
					return Fail(OutError, std::format("Static-mesh payload LOD {} sections do not cover its complete index buffer.", LODIndex));
			}
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
			MaterialSlots.WriteU32(static_cast<uint32>(Payload.MaterialSlotIds.size()));
			for (const FGuid& SlotId : Payload.MaterialSlotIds)
			{
				MaterialSlots.WriteU32(SlotId.A);
				MaterialSlots.WriteU32(SlotId.B);
				MaterialSlots.WriteU32(SlotId.C);
				MaterialSlots.WriteU32(SlotId.D);
			}
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
				return Fail(OutError, "Static-mesh bounds chunk is malformed.");

			FPayloadReader MaterialSlots(Chunks[1]);
			uint32 MaterialSlotCount = 0;
			if (!MaterialSlots.ReadU32(MaterialSlotCount) || MaterialSlotCount == 0 || MaterialSlotCount > MaximumStaticMeshMaterialSlots)
				return Fail(OutError, "Static-mesh material-slot chunk has an invalid count.");
			if (Chunks[1].size() != 4ull + static_cast<uint64>(MaterialSlotCount) * 16ull)
				return Fail(OutError, "Static-mesh material-slot chunk has an invalid size.");
			Payload.MaterialSlotIds.resize(MaterialSlotCount);
			for (FGuid& SlotId : Payload.MaterialSlotIds)
			{
				if (!MaterialSlots.ReadU32(SlotId.A) || !MaterialSlots.ReadU32(SlotId.B)
					|| !MaterialSlots.ReadU32(SlotId.C) || !MaterialSlots.ReadU32(SlotId.D) || !SlotId.IsValid())
					return Fail(OutError, "Static-mesh material-slot chunk contains an invalid identifier.");
			}

			FPayloadReader LODs(Chunks[2]);
			uint32 LODCount = 0;
			if (!LODs.ReadU32(LODCount) || LODCount == 0 || LODCount > MaximumStaticMeshLODs)
				return Fail(OutError, "Static-mesh LOD chunk has an invalid count.");
			if (Chunks[2].size() != 4ull + static_cast<uint64>(LODCount) * 40ull)
				return Fail(OutError, "Static-mesh LOD chunk has an invalid size.");
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
					|| !ReadBounds(LODs, LOD.LocalBounds))
					return Fail(OutError, "Static-mesh LOD chunk is truncated.");
				if (VertexCount == 0 || VertexCount > MaximumStaticMeshVerticesPerLOD
					|| IndexCount == 0 || IndexCount > MaximumStaticMeshIndicesPerLOD
					|| SectionCount == 0 || SectionCount > MaximumStaticMeshSectionsPerLOD
					|| LOD.NumTexCoords > MaxStaticMeshUVChannels
					|| (Flags & ~1u) != 0 || Reserved != 0)
					return Fail(OutError, "Static-mesh LOD chunk contains an invalid count, flag, or reserved value.");
				LOD.bHasVertexColors = (Flags & 1u) != 0;

				const uint64 VertexStride = 40ull + static_cast<uint64>(LOD.NumTexCoords) * 8ull
					+ (LOD.bHasVertexColors ? 16ull : 0ull);
				const uint64 SectionBytes = 4ull + static_cast<uint64>(SectionCount) * 44ull;
				const uint64 VertexBytes = static_cast<uint64>(VertexCount) * VertexStride;
				const uint64 IndexBytes = static_cast<uint64>(IndexCount) * 4ull;
				if (SectionBytes > MaximumStaticMeshPayloadBytes - ExpectedSectionBytes
					|| VertexBytes > MaximumStaticMeshPayloadBytes - ExpectedVertexBytes
					|| IndexBytes > MaximumStaticMeshPayloadBytes - ExpectedIndexBytes)
					return Fail(OutError, "Static-mesh payload stream sizes exceed the allocation limit.");
				ExpectedSectionBytes += SectionBytes;
				ExpectedVertexBytes += VertexBytes;
				ExpectedIndexBytes += IndexBytes;
			}
			if (Chunks[3].size() != ExpectedSectionBytes || Chunks[4].size() != ExpectedVertexBytes
				|| Chunks[5].size() != ExpectedIndexBytes)
				return Fail(OutError, "Static-mesh payload stream chunk sizes do not match the LOD metadata.");
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
					return Fail(OutError, "Static-mesh section chunk count does not match its LOD.");
				for (FStaticMeshPayloadSection& Section : LOD.Sections)
				{
					if (!Sections.ReadU32(Section.FirstIndex) || !Sections.ReadU32(Section.IndexCount)
						|| !Sections.ReadU32(Section.MinVertexIndex) || !Sections.ReadU32(Section.MaxVertexIndex)
						|| !Sections.ReadU32(Section.MaterialSlotIndex) || !ReadBounds(Sections, Section.LocalBounds))
						return Fail(OutError, "Static-mesh section chunk is truncated.");
				}
			}
			if (!Sections.IsAtEnd()) return Fail(OutError, "Static-mesh section chunk contains trailing bytes.");

			FPayloadReader VertexStreams(Chunks[4]);
			auto ReadVector = [&VertexStreams]<typename TVector>(TVector& Value, uint32 ComponentCount) -> bool
			{
				for (uint32 Component = 0; Component < ComponentCount; ++Component)
					if (!VertexStreams.ReadFloat(Value[Component]) || !std::isfinite(Value[Component])) return false;
				return true;
			};
			for (FStaticMeshPayloadLOD& LOD : Payload.LODs)
			{
				for (FVector3f& Value : LOD.Positions) if (!ReadVector(Value, 3)) return Fail(OutError, "Static-mesh positions contain invalid data.");
				for (FVector3f& Value : LOD.Normals) if (!ReadVector(Value, 3)) return Fail(OutError, "Static-mesh normals contain invalid data.");
				for (FVector4f& Value : LOD.Tangents) if (!ReadVector(Value, 4)) return Fail(OutError, "Static-mesh tangents contain invalid data.");
				for (uint32 Channel = 0; Channel < LOD.NumTexCoords; ++Channel)
					for (FVector2f& Value : LOD.TexCoords[Channel])
						if (!ReadVector(Value, 2)) return Fail(OutError, "Static-mesh UVs contain invalid data.");
				for (FVector4f& Value : LOD.Colors) if (!ReadVector(Value, 4)) return Fail(OutError, "Static-mesh colors contain invalid data.");
			}
			if (!VertexStreams.IsAtEnd()) return Fail(OutError, "Static-mesh vertex-stream chunk contains trailing bytes.");

			FPayloadReader IndexBuffers(Chunks[5]);
			for (FStaticMeshPayloadLOD& LOD : Payload.LODs)
				for (uint32& Index : LOD.Indices)
					if (!IndexBuffers.ReadU32(Index)) return Fail(OutError, "Static-mesh index-buffer chunk is truncated.");
			if (!IndexBuffers.IsAtEnd()) return Fail(OutError, "Static-mesh index-buffer chunk contains trailing bytes.");

			if (!ValidatePayload(Payload, OutError)) return false;
			OutPayload = std::move(Payload);
			return true;
		}

		auto ReadU32At(std::span<const uint8> Bytes, size_t Offset, uint32& Value) -> bool
		{
			if (Offset > Bytes.size() || Bytes.size() - Offset < 4) return false;
			Value = 0;
			for (uint32 Byte = 0; Byte < 4; ++Byte) Value |= static_cast<uint32>(Bytes[Offset + Byte]) << (Byte * 8);
			return true;
		}

		auto ReadU64At(std::span<const uint8> Bytes, size_t Offset, uint64& Value) -> bool
		{
			if (Offset > Bytes.size() || Bytes.size() - Offset < 8) return false;
			Value = 0;
			for (uint32 Byte = 0; Byte < 8; ++Byte) Value |= static_cast<uint64>(Bytes[Offset + Byte]) << (Byte * 8);
			return true;
		}

		auto IsRequiredChunkType(uint32 Type) -> bool
		{
			return Type >= static_cast<uint32>(EStaticMeshPayloadChunkType::Bounds)
				&& Type <= static_cast<uint32>(EStaticMeshPayloadChunkType::IndexBuffers);
		}

		auto AlignPayloadOffset(uint64 Offset) -> uint64
		{
			return (Offset + StaticMeshPayloadAlignment - 1) & ~(static_cast<uint64>(StaticMeshPayloadAlignment) - 1);
		}
	}

	auto BuildStaticMeshDerivedDataKeyBytes(
		const FStaticMeshDerivedDataKeyInput& Input) -> std::vector<uint8>
	{
		DerivedDataCache::FWriter Writer;
		Writer.WriteU32(StaticMeshDerivedDataKeySchemaVersion);
		Writer.WriteU64(Input.SourceContentHash.HashLow);
		Writer.WriteU64(Input.SourceContentHash.HashHigh);
		Writer.WriteString(Input.ImporterId);
		Writer.WriteU32(Input.ImporterVersion);
		Writer.WriteU8(static_cast<uint8>(Input.ImportSettings.ForwardAxis));
		Writer.WriteU8(static_cast<uint8>(Input.ImportSettings.RightAxis));
		Writer.WriteU8(static_cast<uint8>(Input.ImportSettings.UpAxis));
		Writer.WriteU32(Input.BuilderVersion);
		Writer.WriteU32(Input.PayloadSchemaVersion);
		Writer.WriteU32(static_cast<uint32>(Input.TargetPlatform));
		return Writer.TakeBytes();
	}

	auto BuildStaticMeshDerivedDataKey(
		const FStaticMeshDerivedDataKeyInput& Input) -> std::string
	{
		return FXxHash128::HashBuffer(BuildStaticMeshDerivedDataKeyBytes(Input)).ToString();
	}

	auto EncodeStaticMeshPayload(
		const FStaticMeshPayloadData& Payload,
		EStaticMeshTargetPlatform TargetPlatform,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool
	{
		OutError.clear();
		if (TargetPlatform != EStaticMeshTargetPlatform::Win64)
			return Fail(OutError, "A concrete target platform is required to encode a static-mesh payload.");
		if (!ValidatePayload(Payload, OutError)) return false;

		const auto ChunkBytes = BuildPayloadChunks(Payload);
		std::array<FStaticMeshPayloadChunk, StaticMeshPayloadRequiredChunkCount> Chunks;
		uint64 Offset = StaticMeshPayloadHeaderSize + StaticMeshPayloadRequiredChunkCount * StaticMeshPayloadChunkEntrySize;
		uint64 TotalUncompressedSize = 0;
		for (uint32 Index = 0; Index < StaticMeshPayloadRequiredChunkCount; ++Index)
		{
			Offset = AlignPayloadOffset(Offset);
			Chunks[Index] = {
				.Type = Index + 1,
				.Flags = StaticMeshChunkFlagRequired,
				.Offset = Offset,
				.StoredSize = ChunkBytes[Index].size(),
				.UncompressedSize = ChunkBytes[Index].size()};
			Offset += ChunkBytes[Index].size();
			TotalUncompressedSize += ChunkBytes[Index].size();
		}

		FPayloadWriter Body;
		for (const FStaticMeshPayloadChunk& Chunk : Chunks)
		{
			Body.WriteU32(Chunk.Type);
			Body.WriteU32(Chunk.Flags);
			Body.WriteU64(Chunk.Offset);
			Body.WriteU64(Chunk.StoredSize);
			Body.WriteU64(Chunk.UncompressedSize);
		}
		uint64 BodyOffset = StaticMeshPayloadHeaderSize + Body.GetBytes().size();
		for (uint32 Index = 0; Index < StaticMeshPayloadRequiredChunkCount; ++Index)
		{
			Body.WriteZeroes(static_cast<size_t>(Chunks[Index].Offset - BodyOffset));
			Body.WriteBytes(ChunkBytes[Index]);
			BodyOffset = Chunks[Index].Offset + Chunks[Index].StoredSize;
		}
		const std::vector<uint8> StoredBody = Body.TakeBytes();

		FPayloadWriter Result;
		Result.WriteU32(StaticMeshPayloadMagic);
		Result.WriteU32(StaticMeshPayloadSchemaVersion);
		Result.WriteU32(StaticMeshBuilderVersion);
		Result.WriteU32(static_cast<uint32>(TargetPlatform));
		Result.WriteU32(0);
		Result.WriteU32(StaticMeshPayloadHeaderSize);
		Result.WriteU32(StaticMeshPayloadRequiredChunkCount);
		Result.WriteU32(0);
		Result.WriteU64(StaticMeshPayloadHeaderSize);
		Result.WriteU64(TotalUncompressedSize);
		Result.WriteU64(Offset);
		Result.WriteU64(FXxHash64::HashBuffer(StoredBody).HashValue);
		Result.WriteBytes(StoredBody);
		OutBytes = Result.TakeBytes();
		return true;
	}

	auto DecodeStaticMeshPayload(
		std::span<const uint8> Bytes,
		EStaticMeshTargetPlatform ExpectedPlatform,
		FStaticMeshPayloadData& OutPayload,
		std::string& OutError) -> bool
	{
		OutError.clear();
		if (Bytes.size() < StaticMeshPayloadHeaderSize) return Fail(OutError, "Static-mesh payload header is truncated.");
		if (ExpectedPlatform != EStaticMeshTargetPlatform::Win64)
			return Fail(OutError, "A concrete target platform is required to decode a static-mesh payload.");

		uint32 Magic = 0;
		uint32 SchemaVersion = 0;
		uint32 BuilderVersion = 0;
		uint32 Platform = 0;
		uint32 PayloadFlags = 0;
		uint32 HeaderSize = 0;
		uint32 ChunkCount = 0;
		uint32 Reserved = 0;
		uint64 ChunkTableOffset = 0;
		uint64 TotalUncompressedSize = 0;
		uint64 StoredSize = 0;
		uint64 StoredHash = 0;
		if (!ReadU32At(Bytes, 0, Magic) || !ReadU32At(Bytes, 4, SchemaVersion)
			|| !ReadU32At(Bytes, 8, BuilderVersion) || !ReadU32At(Bytes, 12, Platform)
			|| !ReadU32At(Bytes, 16, PayloadFlags) || !ReadU32At(Bytes, 20, HeaderSize)
			|| !ReadU32At(Bytes, 24, ChunkCount) || !ReadU32At(Bytes, 28, Reserved)
			|| !ReadU64At(Bytes, 32, ChunkTableOffset) || !ReadU64At(Bytes, 40, TotalUncompressedSize)
			|| !ReadU64At(Bytes, 48, StoredSize) || !ReadU64At(Bytes, 56, StoredHash))
			return Fail(OutError, "Static-mesh payload header is truncated.");
		if (Magic != StaticMeshPayloadMagic) return Fail(OutError, "Static-mesh payload magic is invalid.");
		if (SchemaVersion != StaticMeshPayloadSchemaVersion) return Fail(OutError, "Static-mesh payload schema version is unsupported.");
		if (BuilderVersion != StaticMeshBuilderVersion) return Fail(OutError, "Static-mesh payload builder version is unsupported.");
		if (Platform != static_cast<uint32>(ExpectedPlatform)) return Fail(OutError, "Static-mesh payload target platform does not match.");
		if ((PayloadFlags & ~StaticMeshPayloadFlagCompressed) != 0 || HeaderSize != StaticMeshPayloadHeaderSize
			|| Reserved != 0 || ChunkTableOffset != StaticMeshPayloadHeaderSize)
			return Fail(OutError, "Static-mesh payload header contains invalid flags, sizes, or reserved values.");
		if (ChunkCount < StaticMeshPayloadRequiredChunkCount || ChunkCount > MaximumStaticMeshPayloadChunks)
			return Fail(OutError, "Static-mesh payload chunk count is invalid.");
		if (StoredSize != Bytes.size() || StoredSize > MaximumStaticMeshPayloadBytes
			|| TotalUncompressedSize > MaximumStaticMeshPayloadBytes)
			return Fail(OutError, "Static-mesh payload stored or uncompressed size is invalid.");
		if (FXxHash64::HashBuffer(Bytes.subspan(StaticMeshPayloadHeaderSize)).HashValue != StoredHash)
			return Fail(OutError, "Static-mesh payload checksum does not match.");

		const uint64 TableSize = static_cast<uint64>(ChunkCount) * StaticMeshPayloadChunkEntrySize;
		const uint64 TableEnd = ChunkTableOffset + TableSize;
		if (TableEnd < ChunkTableOffset || TableEnd > StoredSize)
			return Fail(OutError, "Static-mesh payload chunk table is outside the stored object.");

		std::vector<FStaticMeshPayloadChunk> Chunks(ChunkCount);
		uint64 UncompressedSum = 0;
		uint64 PreviousEnd = TableEnd;
		bool bHasCompressedChunk = false;
		std::array<int32, StaticMeshPayloadRequiredChunkCount> RequiredChunkIndices;
		RequiredChunkIndices.fill(-1);
		for (uint32 Index = 0; Index < ChunkCount; ++Index)
		{
			const size_t EntryOffset = static_cast<size_t>(ChunkTableOffset + static_cast<uint64>(Index) * StaticMeshPayloadChunkEntrySize);
			FStaticMeshPayloadChunk& Chunk = Chunks[Index];
			if (!ReadU32At(Bytes, EntryOffset, Chunk.Type) || !ReadU32At(Bytes, EntryOffset + 4, Chunk.Flags)
				|| !ReadU64At(Bytes, EntryOffset + 8, Chunk.Offset) || !ReadU64At(Bytes, EntryOffset + 16, Chunk.StoredSize)
				|| !ReadU64At(Bytes, EntryOffset + 24, Chunk.UncompressedSize))
				return Fail(OutError, "Static-mesh payload chunk table is truncated.");
			if ((Chunk.Flags & ~StaticMeshChunkKnownFlags) != 0)
				return Fail(OutError, "Static-mesh payload chunk contains unsupported flags.");
			const uint32 Compression = (Chunk.Flags & StaticMeshChunkCompressionMask) >> 8;
			if (Compression > StaticMeshChunkCompressionZstandard)
				return Fail(OutError, "Static-mesh payload chunk uses an unsupported compression method.");
			if (Chunk.Offset % StaticMeshPayloadAlignment != 0 || Chunk.Offset < PreviousEnd)
				return Fail(OutError, "Static-mesh payload chunks are misaligned, unordered, or overlapping.");
			if (Chunk.Offset > StoredSize || Chunk.StoredSize > StoredSize - Chunk.Offset)
				return Fail(OutError, "Static-mesh payload chunk range is outside the stored object.");
			if (Chunk.UncompressedSize > MaximumStaticMeshPayloadBytes - UncompressedSum)
				return Fail(OutError, "Static-mesh payload total uncompressed size exceeds its allocation limit.");
			if (Compression == StaticMeshChunkCompressionNone && Chunk.StoredSize != Chunk.UncompressedSize)
				return Fail(OutError, "An uncompressed static-mesh chunk has inconsistent sizes.");
			if (Compression != StaticMeshChunkCompressionNone)
			{
				bHasCompressedChunk = true;
				if (Chunk.StoredSize == 0 || Chunk.UncompressedSize / Chunk.StoredSize > StaticMeshMaximumCompressionRatio
					|| (Chunk.UncompressedSize / Chunk.StoredSize == StaticMeshMaximumCompressionRatio
						&& Chunk.UncompressedSize % Chunk.StoredSize != 0))
					return Fail(OutError, "Static-mesh payload chunk exceeds the maximum compression ratio.");
			}
			for (uint64 PaddingOffset = PreviousEnd; PaddingOffset < Chunk.Offset; ++PaddingOffset)
				if (Bytes[static_cast<size_t>(PaddingOffset)] != 0)
					return Fail(OutError, "Static-mesh payload contains non-zero alignment padding.");
			PreviousEnd = Chunk.Offset + Chunk.StoredSize;
			UncompressedSum += Chunk.UncompressedSize;

			if (IsRequiredChunkType(Chunk.Type))
			{
				const uint32 RequiredIndex = Chunk.Type - 1;
				if ((Chunk.Flags & StaticMeshChunkFlagRequired) == 0 || RequiredChunkIndices[RequiredIndex] >= 0)
					return Fail(OutError, "Static-mesh payload required chunks are missing flags or duplicated.");
				RequiredChunkIndices[RequiredIndex] = static_cast<int32>(Index);
			}
			else if ((Chunk.Flags & StaticMeshChunkFlagRequired) != 0)
			{
				return Fail(OutError, "Static-mesh payload contains an unknown required chunk.");
			}
		}
		if (UncompressedSum != TotalUncompressedSize)
			return Fail(OutError, "Static-mesh payload uncompressed size does not match its chunks.");
		if (bHasCompressedChunk != ((PayloadFlags & StaticMeshPayloadFlagCompressed) != 0))
			return Fail(OutError, "Static-mesh payload compression flags are inconsistent.");
		if (std::ranges::any_of(RequiredChunkIndices, [](int32 Index) { return Index < 0; }))
			return Fail(OutError, "Static-mesh payload is missing a required chunk.");
		for (uint64 PaddingOffset = PreviousEnd; PaddingOffset < StoredSize; ++PaddingOffset)
			if (Bytes[static_cast<size_t>(PaddingOffset)] != 0)
				return Fail(OutError, "Static-mesh payload contains non-zero trailing padding.");
		if (bHasCompressedChunk)
			return Fail(OutError, "Compressed static-mesh chunks are not supported by this build.");

		std::array<std::span<const uint8>, StaticMeshPayloadRequiredChunkCount> RequiredChunks;
		for (uint32 Index = 0; Index < StaticMeshPayloadRequiredChunkCount; ++Index)
		{
			const FStaticMeshPayloadChunk& Chunk = Chunks[static_cast<size_t>(RequiredChunkIndices[Index])];
			RequiredChunks[Index] = Bytes.subspan(static_cast<size_t>(Chunk.Offset), static_cast<size_t>(Chunk.StoredSize));
		}
		FStaticMeshPayloadData Decoded;
		if (!ReadPayloadChunks(RequiredChunks, Decoded, OutError)) return false;
		OutPayload = std::move(Decoded);
		return true;
	}

	auto MakeStaticMeshPayloadData(
		const FStaticMeshRenderData& RenderData,
		FStaticMeshPayloadData& OutPayload,
		std::string& OutError) -> bool
	{
		FStaticMeshPayloadData Payload;
		Payload.LocalBounds = RenderData.LocalBounds;
		Payload.MaterialSlotIds.reserve(RenderData.MaterialSlots.size());
		for (const FStaticMeshMaterialSlot& Slot : RenderData.MaterialSlots) Payload.MaterialSlotIds.push_back(Slot.SlotId);
		Payload.LODs.reserve(RenderData.LODResources.size());
		for (const FStaticMeshLODResources& SourceLOD : RenderData.LODResources)
		{
			FStaticMeshPayloadLOD& LOD = Payload.LODs.emplace_back();
			LOD.Positions = SourceLOD.Positions;
			LOD.Normals = SourceLOD.Normals;
			LOD.Tangents = SourceLOD.Tangents;
			LOD.Indices = SourceLOD.Indices;
			LOD.LocalBounds = SourceLOD.LocalBounds;
			LOD.NumTexCoords = SourceLOD.NumTexCoords;
			LOD.bHasVertexColors = SourceLOD.bHasVertexColors;
			for (uint32 Channel = 0; Channel < LOD.NumTexCoords; ++Channel)
				LOD.TexCoords[Channel] = SourceLOD.TexCoords[Channel];
			if (LOD.bHasVertexColors) LOD.Colors = SourceLOD.Colors;
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
		RenderData->MaterialSlots.reserve(Payload.MaterialSlotIds.size());
		for (const FGuid& SlotId : Payload.MaterialSlotIds) RenderData->MaterialSlots.push_back({.SlotId = SlotId});
		RenderData->LODResources.reserve(Payload.LODs.size());
		for (const FStaticMeshPayloadLOD& SourceLOD : Payload.LODs)
		{
			FStaticMeshLODResources& LOD = RenderData->LODResources.emplace_back();
			LOD.Positions = SourceLOD.Positions;
			LOD.Normals = SourceLOD.Normals;
			LOD.Tangents = SourceLOD.Tangents;
			LOD.Indices = SourceLOD.Indices;
			LOD.LocalBounds = SourceLOD.LocalBounds;
			LOD.NumTexCoords = SourceLOD.NumTexCoords;
			LOD.bHasVertexColors = SourceLOD.bHasVertexColors;
			const size_t VertexCount = LOD.Positions.size();
			for (uint32 Channel = 0; Channel < MaxStaticMeshUVChannels; ++Channel)
			{
				LOD.TexCoords[Channel] = Channel < LOD.NumTexCoords
					? SourceLOD.TexCoords[Channel]
					: std::vector<FVector2f>(VertexCount, FVector2f(0.0f));
			}
			LOD.Colors = LOD.bHasVertexColors
				? SourceLOD.Colors
				: std::vector<FVector4f>(VertexCount, FVector4f(1.0f));
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
}
