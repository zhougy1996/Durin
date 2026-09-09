#include "StaticMesh/StaticMeshDerivedData.h"

#include "Asset/ChunkedPayload.h"
#include "Serialization/Archive.h"
#include "Serialization/BinaryFormat.h"



namespace Durin
{
	namespace
	{
		auto ResolveMeshTarget(FArchive& Ar, EStaticMeshTargetPlatform& TargetPlatform) -> bool
		{
			if (TargetPlatform == EStaticMeshTargetPlatform::Unknown && Ar.GetTarget().Platform == "Win64")
				TargetPlatform = EStaticMeshTargetPlatform::Win64;
			if (TargetPlatform != EStaticMeshTargetPlatform::Win64
				|| (!Ar.GetTarget().Platform.empty() && Ar.GetTarget().Platform != "Win64"))
			{
				Ar.Fail(EArchiveFailureCode::UnsupportedTarget, "Static-mesh target context is unsupported or conflicting.");
				return false;
			}
			return true;
		}

		struct FPayloadBuildCancelled {};

		// Borrowed for this call only; cancellation never crosses the public codec boundary.
		struct FPayloadBuildControl
		{
			const std::function<bool()>& ShouldCancel;
			uint32 WorkSinceCheckpoint = 0;
			auto Check() const -> void
			{
				if (ShouldCancel && ShouldCancel()) throw FPayloadBuildCancelled{};
			}
			auto Tick() -> void
			{
				if (++WorkSinceCheckpoint < 256) return;
				WorkSinceCheckpoint = 0;
				Check();
			}
		};

		inline constexpr uint32 StaticMeshPayloadRequiredChunkCount = 6;
		inline constexpr uint32 StaticMeshPayloadFlagCompressed = 1;
		inline constexpr uint32 StaticMeshChunkCompressionMask = 0x0000ff00;
		inline constexpr uint32 StaticMeshChunkCompressionZstandard = 1;
		inline constexpr uint64 StaticMeshMaximumCompressionRatio = 64;


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

		auto ValidatePayload(const FStaticMeshPayloadData& Payload, std::string& OutError, FPayloadBuildControl& Control) -> bool
		{
			if (!IsValidBounds(Payload.LocalBounds)) return Fail("Static-mesh payload bounds are invalid or not exactly representable as float32.", &OutError);
			if (Payload.MaterialSlotCount == 0 || Payload.MaterialSlotCount > MaximumMeshMaterialSlots)
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
				Control.Tick();
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
					Control.Tick();
					const size_t ExpectedCount = Channel < LOD.NumTexCoords ? VertexCount : 0;
					if (LOD.TexCoords[Channel].size() != ExpectedCount)
						return Fail(std::format(
							"Static-mesh payload LOD {} UV stream {} has {} values; expected {}.",
							LODIndex, Channel, LOD.TexCoords[Channel].size(), ExpectedCount), &OutError);
				}
				if (LOD.Colors.size() != (LOD.bHasVertexColors ? VertexCount : 0))
					return Fail(std::format("Static-mesh payload LOD {} color-stream count does not match its flags.", LODIndex), &OutError);
				if (std::ranges::any_of(LOD.Positions, [&Control](const FVector3f& Value) { Control.Tick(); return !IsFinite(Value); })
					|| std::ranges::any_of(LOD.Normals, [&Control](const FVector3f& Value) { Control.Tick(); return !IsFinite(Value); })
					|| std::ranges::any_of(LOD.Tangents, [&Control](const FVector4f& Value) { Control.Tick(); return !IsFinite(Value); })
					|| std::ranges::any_of(LOD.Colors, [&Control](const FVector4f& Value) { Control.Tick(); return !IsFinite(Value); }))
					return Fail(std::format("Static-mesh payload LOD {} contains a non-finite vertex attribute.", LODIndex), &OutError);
				for (uint32 Channel = 0; Channel < LOD.NumTexCoords; ++Channel)
				{
					Control.Tick();
					if (std::ranges::any_of(LOD.TexCoords[Channel], [&Control](const FVector2f& Value) { Control.Tick(); return !IsFinite(Value); }))
						return Fail(std::format("Static-mesh payload LOD {} contains a non-finite UV.", LODIndex), &OutError);
				}
				if (std::ranges::any_of(LOD.Indices, [VertexCount, &Control](uint32 Index) { Control.Tick(); return Index >= VertexCount; }))
					return Fail(std::format("Static-mesh payload LOD {} contains an out-of-range index.", LODIndex), &OutError);

				uint64 CoveredIndices = 0;
				for (const FStaticMeshPayloadSection& Section : LOD.Sections)
				{
					Control.Tick();
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
						Control.Tick();
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

		auto SerializeBounds(FArchive& Ar, FBox& Bounds) -> void
		{
			std::array<float, 6> Values{};
			if (Ar.IsSaving())
				Values = {static_cast<float>(Bounds.Min.x), static_cast<float>(Bounds.Min.y),
					static_cast<float>(Bounds.Min.z), static_cast<float>(Bounds.Max.x),
					static_cast<float>(Bounds.Max.y), static_cast<float>(Bounds.Max.z)};
			for (float& Value : Values) Ar << Value;
			if (Ar.IsLoading() && !Ar.HasError())
			{
				Bounds = FBox(FVector3(Values[0], Values[1], Values[2]), FVector3(Values[3], Values[4], Values[5]));
				if (!IsValidBounds(Bounds)) Ar.Fail(EArchiveFailureCode::InvalidData, "Static-mesh bounds are invalid.");
			}
		}

		// One schema for the six logical streams. Metadata is validated against
		// exact chunk extents and native allocation totals before resizing streams.
		auto SerializePayloadChunks(const std::array<FArchive*, 6>& Chunks,
			FStaticMeshPayloadData& Payload, FPayloadBuildControl& Control) -> void
		{
			FArchive& Metadata = *Chunks[2];
			const bool Loading = Metadata.IsLoading();
			SerializeBounds(*Chunks[0], Payload.LocalBounds);
			*Chunks[1] << Payload.MaterialSlotCount;
			uint32 LODCount = Loading ? 0 : static_cast<uint32>(Payload.LODs.size());
			Metadata << LODCount;
			if (Metadata.HasError()) return;
			if (LODCount == 0 || LODCount > MaximumStaticMeshLODs)
			{
				Metadata.Fail(EArchiveFailureCode::LimitExceeded, "Static-mesh LOD count exceeds its limit.");
				return;
			}
			if (Loading)
			{
				if (Metadata.GetRemainingPayloadBytes() != static_cast<uint64>(LODCount) * 44)
				{
					Metadata.Fail(EArchiveFailureCode::InvalidData, "Static-mesh LOD metadata size is invalid.");
					return;
				}
				Payload.LODs.clear();
				Payload.LODs.resize(LODCount);
			}
			std::array<uint32, MaximumStaticMeshLODs> Vertices{}, Indices{}, Sections{};
			uint64 SectionBytes = 0, VertexBytes = 0, IndexBytes = 0, NativeBytes = 0;
			for (uint32 Index = 0; Index < LODCount; ++Index)
			{
				Control.Tick();
				auto& LOD = Payload.LODs[Index];
				if (!Loading)
				{
					Vertices[Index] = static_cast<uint32>(LOD.Positions.size());
					Indices[Index] = static_cast<uint32>(LOD.Indices.size());
					Sections[Index] = static_cast<uint32>(LOD.Sections.size());
				}
				uint8 Flags = LOD.bHasVertexColors ? 1 : 0;
				uint16 Reserved = 0;
				Metadata << Vertices[Index] << Indices[Index] << Sections[Index]
					<< LOD.NumTexCoords << Flags << Reserved << LOD.ScreenSize;
				SerializeBounds(Metadata, LOD.LocalBounds);
				if (Metadata.HasError()) return;
				if (Vertices[Index] == 0 || Vertices[Index] > MaximumStaticMeshVerticesPerLOD
					|| Indices[Index] == 0 || Indices[Index] > MaximumStaticMeshIndicesPerLOD
					|| Sections[Index] == 0 || Sections[Index] > MaximumStaticMeshSectionsPerLOD
					|| LOD.NumTexCoords > MaxStaticMeshUVChannels || (Flags & ~1u) != 0 || Reserved != 0)
				{
					Metadata.Fail(EArchiveFailureCode::LimitExceeded, "Static-mesh LOD count or flags are invalid.");
					return;
				}
				if (Loading) LOD.bHasVertexColors = (Flags & 1) != 0;
				const uint64 Stride = 40ull + LOD.NumTexCoords * 8ull + (LOD.bHasVertexColors ? 16ull : 0);
				SectionBytes += 4ull + Sections[Index] * 44ull;
				VertexBytes += Vertices[Index] * Stride;
				IndexBytes += Indices[Index] * 4ull;
				NativeBytes += static_cast<uint64>(Vertices[Index]) * (sizeof(FVector3f) * 2
					+ sizeof(FVector4f) + LOD.NumTexCoords * sizeof(FVector2f)
					+ (LOD.bHasVertexColors ? sizeof(FVector4f) : 0))
					+ static_cast<uint64>(Indices[Index]) * sizeof(uint32)
					+ static_cast<uint64>(Sections[Index]) * sizeof(FStaticMeshPayloadSection);
			}
			if (NativeBytes > MaximumStaticMeshPayloadBytes
				|| SectionBytes + VertexBytes + IndexBytes > MaximumStaticMeshPayloadBytes)
			{
				Metadata.Fail(EArchiveFailureCode::LimitExceeded, "Static-mesh decoded allocation budget is exceeded.");
				return;
			}
			if (Loading && (Chunks[3]->GetRemainingPayloadBytes() != SectionBytes
				|| Chunks[4]->GetRemainingPayloadBytes() != VertexBytes
				|| Chunks[5]->GetRemainingPayloadBytes() != IndexBytes))
			{
				Metadata.Fail(EArchiveFailureCode::InvalidData, "Static-mesh stream sizes do not match LOD metadata.");
				return;
			}
			for (uint32 Index = 0; Index < LODCount; ++Index)
			{
				auto& LOD = Payload.LODs[Index];
				Control.Check();
				if (Loading)
				{
					LOD.Positions.resize(Vertices[Index]);
					LOD.Normals.resize(Vertices[Index]);
					LOD.Tangents.resize(Vertices[Index]);
					for (uint32 Channel = 0; Channel < LOD.NumTexCoords; ++Channel)
						LOD.TexCoords[Channel].resize(Vertices[Index]);
					if (LOD.bHasVertexColors) LOD.Colors.resize(Vertices[Index]);
					LOD.Indices.resize(Indices[Index]);
					LOD.Sections.resize(Sections[Index]);
				}
				uint32 SectionCount = Sections[Index];
				*Chunks[3] << SectionCount;
				if (SectionCount != Sections[Index])
				{
					Chunks[3]->Fail(EArchiveFailureCode::InvalidData, "Static-mesh section count disagrees with metadata.");
					return;
				}
				for (auto& Section : LOD.Sections)
				{
					Control.Tick();
					*Chunks[3] << Section.FirstIndex << Section.IndexCount << Section.MinVertexIndex
						<< Section.MaxVertexIndex << Section.MaterialSlotIndex;
					SerializeBounds(*Chunks[3], Section.LocalBounds);
				}
				auto Transfer = [&]<typename T>(std::vector<T>& Values, uint32 Components) {
					for (auto& Value : Values)
						for (uint32 Component = 0; Component < Components; ++Component)
						{
							Control.Tick();
							*Chunks[4] << Value[Component];
						}
				};
				Transfer(LOD.Positions, 3);
				Transfer(LOD.Normals, 3);
				Transfer(LOD.Tangents, 4);
				for (uint32 Channel = 0; Channel < LOD.NumTexCoords; ++Channel) Transfer(LOD.TexCoords[Channel], 2);
				if (LOD.bHasVertexColors) Transfer(LOD.Colors, 4);
				for (uint32& Value : LOD.Indices) { Control.Tick(); *Chunks[5] << Value; }
			}
			if (Loading) for (FArchive* Chunk : Chunks) RequireArchiveEnd(*Chunk);
		}

		// The physical container supplies the declared extent; borrowing keeps
		// its backing memory alive in the caller through all chunk interpretation.
		auto ReadMeshRegion(FArchive& Ar, uint32 SizeOffset, uint64 MaximumBytes, FByteView& Bytes) -> bool
		{
			FByteView Header;
			if (!Ar.ReadRegion(64, Header)) return false;
			uint64 Size = 0;
			ReadLittleEndianAt(Header, SizeOffset, Size);
			if (Size < 64 || Size > MaximumBytes)
			{
				Ar.Fail(EArchiveFailureCode::LimitExceeded, "Mesh payload stored size is outside its limit.");
				return false;
			}
			FByteView Body;
			if (!Ar.ReadRegion(Size - 64, Body)) return false;
			if (Body.data() != Header.data() + 64)
			{
				Ar.Fail(EArchiveFailureCode::UnsupportedCapability, "Mesh payload requires contiguous borrowed regions.");
				return false;
			}
			Bytes = FByteView(Header.data(), static_cast<size_t>(Size));
			return true;
		}

		auto ArchiveChunkFailure(const FChunkedPayloadResult& Result) -> EArchiveFailureCode
		{
			if (Result.Kind == EChunkedPayloadFailureKind::Incompatible)
				return EArchiveFailureCode::UnsupportedVersion;
			switch (Result.Failure)
			{
			case EChunkedPayloadFailure::TruncatedHeader: return EArchiveFailureCode::TruncatedPayload;
			case EChunkedPayloadFailure::InvalidChunkCount:
			case EChunkedPayloadFailure::CompressionRatioExceeded: return EArchiveFailureCode::LimitExceeded;
			case EChunkedPayloadFailure::NonzeroPadding: return EArchiveFailureCode::NonZeroPadding;
			case EChunkedPayloadFailure::TrailingData: return EArchiveFailureCode::TrailingData;
			default: return EArchiveFailureCode::InvalidData;
			}
		}

		auto GetStaticMeshChunkedPayloadFormat() -> FChunkedPayloadFormat
		{
			static_assert(StaticMeshPayloadHeaderSize == ChunkedPayloadHeaderSize);
			static_assert(StaticMeshPayloadChunkEntrySize == ChunkedPayloadEntrySize);
			return {
				.HeaderSizeWordIndex = 5,
				.ChunkCountWordIndex = 6,
				.GlobalFlagsWordIndex = 4,
				.GlobalCompressedFlag = StaticMeshPayloadFlagCompressed,
				.RequiredChunkCount = StaticMeshPayloadRequiredChunkCount,
				.MaximumChunkCount = MaximumStaticMeshPayloadChunks,
				.RequiredChunkFlag = ChunkedPayloadRequiredFlag,
				.KnownChunkFlags = ChunkedPayloadRequiredFlag | StaticMeshChunkCompressionMask,
				.CompressionMask = StaticMeshChunkCompressionMask,
				.CompressionShift = 8,
				.MaximumCompressionMethod = StaticMeshChunkCompressionZstandard,
				.MaximumCompressionRatio = StaticMeshMaximumCompressionRatio,
				.MaximumBytes = MaximumStaticMeshPayloadBytes,
				.Alignment = StaticMeshPayloadAlignment,
				.AllowTrailingZeroPadding = true};
		}


	}

	auto MakeStaticMeshPayloadData(
		const FStaticMeshRenderData& RenderData,
		FStaticMeshPayloadData& OutPayload,
		std::string& OutError,
		const std::function<bool()>& ShouldCancel) -> bool
	try
	{
		FPayloadBuildControl Control{ShouldCancel};
		Control.Check();
		FStaticMeshPayloadData Payload;
		Payload.LocalBounds = RenderData.LocalBounds;
		Payload.MaterialSlotCount = static_cast<uint32>(RenderData.MaterialSlots.size());
		Payload.LODs.reserve(RenderData.LODResources.size());
		for (const FStaticMeshLODResources& SourceLOD : RenderData.LODResources)
		{
			Control.Tick();
			FStaticMeshPayloadLOD& LOD = Payload.LODs.emplace_back();
			Control.Check();
			LOD.Positions =
				SourceLOD.VertexBuffers.PositionVertexBuffer
					.GetPositions();
			Control.Check();
			LOD.Normals =
				SourceLOD.VertexBuffers.StaticMeshVertexBuffer
					.TangentsVertexBuffer.GetNormals();
			Control.Check();
			LOD.Tangents =
				SourceLOD.VertexBuffers.StaticMeshVertexBuffer
					.TangentsVertexBuffer.GetTangents();
			Control.Check();
			LOD.Indices = SourceLOD.IndexBuffer.GetIndices();
			LOD.LocalBounds = SourceLOD.LocalBounds;
			LOD.ScreenSize = SourceLOD.ScreenSize;
			LOD.NumTexCoords = SourceLOD.NumTexCoords;
			if (LOD.NumTexCoords > MaxStaticMeshUVChannels)
				return Fail("Static-mesh runtime UV-channel count is invalid.", &OutError);
			LOD.bHasVertexColors =
				SourceLOD.bHasColorVertexData;
			const auto& SourceTexCoords =
				SourceLOD.VertexBuffers.StaticMeshVertexBuffer
					.TexCoordVertexBuffer.GetTexCoords();
			for (uint32 Channel = 0; Channel < LOD.NumTexCoords; ++Channel)
			{
				Control.Tick();
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
				Control.Tick();
				LOD.Sections.push_back({
					.FirstIndex = SourceSection.FirstIndex,
					.IndexCount = SourceSection.IndexCount,
					.MinVertexIndex = SourceSection.MinVertexIndex,
					.MaxVertexIndex = SourceSection.MaxVertexIndex,
					.MaterialSlotIndex = SourceSection.MaterialSlotIndex,
					.LocalBounds = SourceSection.LocalBounds});
			}
		}
		if (!ValidatePayload(Payload, OutError, Control)) return false;
		Control.Check();
		OutPayload = std::move(Payload);
		return true;
	}
	catch (const FPayloadBuildCancelled&)
	{
		return Fail("StaticMesh payload operation was cancelled.", &OutError);
	}

	auto MakeStaticMeshRenderData(
		const FStaticMeshPayloadData& Payload,
		std::unique_ptr<FStaticMeshRenderData>& OutRenderData,
		std::string& OutError,
		const std::function<bool()>& ShouldCancel) -> bool
	try
	{
		FPayloadBuildControl Control{ShouldCancel};
		Control.Check();
		if (!ValidatePayload(Payload, OutError, Control)) return false;
		auto RenderData = std::make_unique<FStaticMeshRenderData>();
		RenderData->LocalBounds = Payload.LocalBounds;
		RenderData->MaterialSlots.resize(Payload.MaterialSlotCount);
		RenderData->LODResources.reserve(Payload.LODs.size());
		for (const FStaticMeshPayloadLOD& SourceLOD : Payload.LODs)
		{
			Control.Tick();
			FStaticMeshLODResources& LOD = RenderData->LODResources.emplace_back();
			Control.Check();
			LOD.VertexBuffers.PositionVertexBuffer.Init(
				SourceLOD.Positions);
			Control.Check();
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
			Control.Check();
			LOD.IndexBuffer.Init(SourceLOD.Indices);
			LOD.LocalBounds = SourceLOD.LocalBounds;
			LOD.ScreenSize = SourceLOD.ScreenSize;
			LOD.NumTexCoords = SourceLOD.NumTexCoords;
			LOD.bHasColorVertexData =
				SourceLOD.bHasVertexColors;
			Control.Check();
			LOD.VertexBuffers.Finalize(
				LOD.NumTexCoords,
				LOD.bHasColorVertexData);
			LOD.Sections.reserve(SourceLOD.Sections.size());
			for (const FStaticMeshPayloadSection& SourceSection : SourceLOD.Sections)
			{
				Control.Tick();
				LOD.Sections.push_back({
					.FirstIndex = SourceSection.FirstIndex,
					.IndexCount = SourceSection.IndexCount,
					.MinVertexIndex = SourceSection.MinVertexIndex,
					.MaxVertexIndex = SourceSection.MaxVertexIndex,
					.MaterialSlotIndex = SourceSection.MaterialSlotIndex,
					.LocalBounds = SourceSection.LocalBounds});
			}
		}
		Control.Check();
		OutRenderData = std::move(RenderData);
		return true;
	}
	catch (const FPayloadBuildCancelled&)
	{
		return Fail("StaticMesh payload operation was cancelled.", &OutError);
	}

	namespace
	{
		auto AlignCollisionOffset(uint64 Offset) -> uint64
		{
			return (Offset + StaticMeshCollisionPayloadAlignment - 1)
				& ~(static_cast<uint64>(StaticMeshCollisionPayloadAlignment) - 1);
		}


	}

	auto MakeStaticMeshCollisionPayloadData(
		const FCollisionGeometryRef& Geometry,
		EBodySetupCollisionQueryPolicy QueryPolicy,
		FStaticMeshCollisionPayloadData& OutPayload,
		std::string& OutError,
		const std::function<bool()>& ShouldCancel) -> bool
	try
	{
		FPayloadBuildControl Control{ShouldCancel};
		Control.Check();
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
			Control.Tick();
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
			Control.Tick();
			const FCollisionGeometryTriangle* Triangle = Geometry.GetTriangle(Index);
			if (!Triangle) return Fail("Collision geometry has an invalid triangle.", &OutError);
			Candidate.Indices.insert(Candidate.Indices.end(),
				{Triangle->First, Triangle->Second, Triangle->Third});
			Candidate.SourceOrdinals.push_back(Triangle->SourceOrdinal);
		}
		Candidate.Nodes.reserve(Geometry.GetNodeCount());
		for (uint32 Index = 0; Index < Geometry.GetNodeCount(); ++Index)
		{
			Control.Tick();
			const FCollisionGeometryNode* Node = Geometry.GetNode(Index);
			if (!Node) return Fail("Collision geometry has an invalid BVH node.", &OutError);
			Candidate.Nodes.push_back(*Node);
		}
		Candidate.LeafTriangles.reserve(Geometry.GetLeafTriangleCount());
		for (uint32 Index = 0; Index < Geometry.GetLeafTriangleCount(); ++Index)
		{
			Control.Tick();
			const uint32 TriangleIndex = Geometry.GetLeafTriangle(Index);
			const FCollisionGeometryTriangle* Triangle = Geometry.GetTriangle(TriangleIndex);
			if (!Triangle) return Fail("Collision geometry has an invalid BVH membership.", &OutError);
			Candidate.LeafTriangles.push_back(Triangle->SourceOrdinal);
		}
		Control.Check();
		OutPayload = std::move(Candidate);
		OutError.clear();
		return true;
	}
	catch (const FPayloadBuildCancelled&)
	{
		return Fail("StaticMesh payload operation was cancelled.", &OutError);
	}

	auto MakeStaticMeshCollisionGeometry(
		const FStaticMeshCollisionPayloadData& Payload,
		FCollisionGeometryRef& OutGeometry,
		std::string& OutError,
		const std::function<bool()>& ShouldCancel) -> bool
	try
	{
		FPayloadBuildControl Control{ShouldCancel};
		Control.Check();
		if (Payload.Positions.empty() || Payload.Indices.empty()
			|| Payload.Indices.size() % 3 != 0
			|| Payload.SourceOrdinals.size() != Payload.Indices.size() / 3)
			return Fail("Collision payload counts are inconsistent.", &OutError);
		std::vector<FVector3> Vertices;
		Vertices.reserve(Payload.Positions.size());
		for (const FVector3f& Position : Payload.Positions)
		{
			Control.Tick();
			if (!Math::IsFinite(Position)) return Fail("Collision payload contains a non-finite position.", &OutError);
			Vertices.emplace_back(Position);
		}
		FCollisionGeometryRef Candidate;
		if (Payload.SourceMode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0)
		{
			if (!Payload.Nodes.empty() || !Payload.LeafTriangles.empty())
				return Fail("Convex collision payload must not contain a BVH.", &OutError);
			Candidate = FCollisionGeometryRef::MakeConvexHull(Vertices, Payload.Indices, ShouldCancel);
		}
		else if (Payload.SourceMode == EBodySetupCollisionSourceMode::TriangleMeshFromLOD0)
		{
			std::map<uint32, uint32> OrdinalToTriangle;
			for (uint32 Triangle = 0; Triangle < Payload.SourceOrdinals.size(); ++Triangle)
			{
				Control.Tick();
				if (!OrdinalToTriangle.emplace(Payload.SourceOrdinals[Triangle], Triangle).second)
					return Fail("Collision payload source ordinals are not unique.", &OutError);
			}
			std::vector<uint32> LeafTriangles;
			LeafTriangles.reserve(Payload.LeafTriangles.size());
			for (uint32 Ordinal : Payload.LeafTriangles)
			{
				Control.Tick();
				const auto Found = OrdinalToTriangle.find(Ordinal);
				if (Found == OrdinalToTriangle.end())
					return Fail("Collision BVH references an unknown source ordinal.", &OutError);
				LeafTriangles.push_back(Found->second);
			}
			Candidate = FCollisionGeometryRef::MakeCookedTriangleMesh(
				Vertices, Payload.Indices, Payload.SourceOrdinals, Payload.Nodes, LeafTriangles, ShouldCancel);
		}
		else return Fail("Collision payload source mode is invalid.", &OutError);
		if (!Candidate) return Fail("Collision payload topology or BVH is invalid.", &OutError);
		Control.Check();
		OutGeometry = Candidate;
		OutError.clear();
		return true;
	}
	catch (const FPayloadBuildCancelled&)
	{
		return Fail("StaticMesh payload operation was cancelled.", &OutError);
	}

	auto FStaticMeshPayloadData::Serialize(FArchive& Ar,
		EStaticMeshTargetPlatform TargetPlatform, const std::function<bool()>& ShouldCancel) -> void
	try
	{
		if (Ar.HasError()) return;
		FPayloadBuildControl Control{ShouldCancel};
		Control.Check();
		if (!ResolveMeshTarget(Ar, TargetPlatform)) return;
		std::string Error;
		std::array<FByteBuffer, 6> Buffers;
		std::array<std::unique_ptr<FArchive>, 6> Owners;
		std::array<FArchive*, 6> Chunks;
		FDecodedChunkedPayload Container;
		if (Ar.IsSaving())
		{
			if (!ValidatePayload(*this, Error, Control))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, Error);
				return;
			}
			for (uint32 Index = 0; Index < 6; ++Index)
				Owners[Index] = std::make_unique<FCanonicalMemoryWriter>(Buffers[Index]);
		}
		else
		{
			FByteView Bytes;
			if (!ReadMeshRegion(Ar, 48, MaximumStaticMeshPayloadBytes, Bytes)) return;
			const auto Result = DecodeChunkedPayload(Bytes, GetStaticMeshChunkedPayloadFormat(), Container);
			if (!Result)
			{
				Ar.Fail(ArchiveChunkFailure(Result),
					DescribeChunkedPayloadFailure(Result.Failure, "Static-mesh payload"));
				return;
			}
			const auto& Header = Container.HeaderWords;
			if (Header[1] != StaticMeshPayloadSchemaVersion || Header[2] != StaticMeshBuilderVersion)
			{
				Ar.Fail(EArchiveFailureCode::UnsupportedVersion, "Static-mesh payload schema or builder version is unsupported.");
				return;
			}
			if (Header[3] != static_cast<uint32>(TargetPlatform))
			{
				Ar.Fail(EArchiveFailureCode::UnsupportedTarget, "Static-mesh payload target does not match.");
				return;
			}
			if (Header[0] != 0 || Header[7] != 0
				|| (Header[4] & ~StaticMeshPayloadFlagCompressed) != 0)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, "Static-mesh reserved header fields are nonzero.");
				return;
			}
			for (uint32 Index = 0; Index < 6; ++Index)
				Owners[Index] = std::make_unique<FCanonicalMemoryReader>(Container.RequiredChunks[Index]);
		}
		for (uint32 Index = 0; Index < 6; ++Index) Chunks[Index] = Owners[Index].get();
		SerializePayloadChunks(Chunks, *this, Control);
		for (FArchive* Chunk : Chunks)
			if (Chunk->HasError())
			{
				Ar.Fail(Chunk->GetFailure()->Code, Chunk->GetFailure()->Message);
				return;
			}
		if (Ar.IsLoading())
		{
			if (!ValidatePayload(*this, Error, Control)) Ar.Fail(EArchiveFailureCode::InvalidData, Error);
			return;
		}
		std::array<FChunkedPayloadInput, 6> Inputs;
		for (uint32 Index = 0; Index < 6; ++Index)
			Inputs[Index] = {.Type = Index + 1, .Flags = ChunkedPayloadRequiredFlag,
				.Bytes = Buffers[Index], .DecodedSize = Buffers[Index].size()};
		FByteBuffer Bytes;
		const auto Result = EncodeChunkedPayload({0, StaticMeshPayloadSchemaVersion, StaticMeshBuilderVersion,
			static_cast<uint32>(TargetPlatform), 0, StaticMeshPayloadHeaderSize, 6, 0},
			Inputs, GetStaticMeshChunkedPayloadFormat(), Bytes);
		Control.Check();
		if (!Result)
			Ar.Fail(EArchiveFailureCode::InvalidData, DescribeChunkedPayloadFailure(Result.Failure, "Static-mesh payload"));
		else Ar.WriteBytes(Bytes);
	}
	catch (const FPayloadBuildCancelled&)
	{
		Ar.Fail(EArchiveFailureCode::InvalidData, "StaticMesh payload operation was cancelled.");
	}

	auto FStaticMeshCollisionPayloadData::Serialize(FArchive& Ar,
		EStaticMeshTargetPlatform TargetPlatform, const std::function<bool()>& ShouldCancel) -> void
	try
	{
		if (Ar.HasError()) return;
		FPayloadBuildControl Control{ShouldCancel};
		Control.Check();
		auto Reject = [&](EArchiveFailureCode Code, std::string_view Message) { Ar.Fail(Code, Message); };
		if (!ResolveMeshTarget(Ar, TargetPlatform)) return;

		uint32 Reserved0 = 0, Schema = StaticMeshCollisionPayloadSchemaVersion;
		uint32 Builder = StaticMeshCollisionBuilderVersion, Platform = static_cast<uint32>(TargetPlatform);
		uint32 Header = StaticMeshCollisionPayloadHeaderSize, ChunkCount = 4;
		uint32 Alignment = StaticMeshCollisionPayloadAlignment, Mode = 0, Policy = 0, Reserved = 0;
		uint64 StoredSize = 0, LogicalBytes = 0, Checksum = 0;
		const std::array<uint64, 4> ElementSizes{12, 4, 4, 32};
		std::array<uint64, 4> Counts{}, Offsets{}, Sizes{};
		std::array<FByteBuffer, 4> Buffers;
		std::array<std::unique_ptr<FArchive>, 4> Streams;
		std::vector<uint32> OrderedIndices, OrderedOrdinals;
		auto* WireIndices = &Indices;
		auto* WireOrdinals = &SourceOrdinals;
		FByteBuffer Body;
		std::string Error;

		auto TransferStreams = [&] {
			for (auto& Position : Positions)
				for (uint32 Axis = 0; Axis < 3; ++Axis) { Control.Tick(); *Streams[0] << Position[Axis]; }
			for (uint32& Index : *WireIndices) { Control.Tick(); *Streams[1] << Index; }
			for (uint32& Ordinal : *WireOrdinals) { Control.Tick(); *Streams[2] << Ordinal; }
			for (auto& Node : Nodes)
			{
				Control.Tick();
				for (uint32 Axis = 0; Axis < 3; ++Axis) *Streams[3] << Node.Minimum[Axis];
				*Streams[3] << Node.First;
				for (uint32 Axis = 0; Axis < 3; ++Axis) *Streams[3] << Node.Maximum[Axis];
				*Streams[3] << Node.CountOrSecond;
			}
		};
		auto TransferTable = [&](FArchive& Table) {
			for (uint32 Chunk = 0; Chunk < 4; ++Chunk)
			{
				uint32 Type = Chunk + 1, Flags = 1;
				Table << Type << Flags << Offsets[Chunk] << Sizes[Chunk] << Counts[Chunk];
				if (Type != Chunk + 1 || Flags != 1)
					Table.Fail(EArchiveFailureCode::InvalidData, "DCOL chunk identity is invalid.");
			}
		};
		if (Ar.IsSaving())
		{
			// Bound all serialization scratch before geometry validation or ordering.
			Counts = {Positions.size(), Indices.size(), SourceOrdinals.size(), Nodes.size()};
			StoredSize = 64 + 4 * StaticMeshCollisionPayloadChunkEntrySize;
			for (uint32 Chunk = 0; Chunk < 4; ++Chunk)
			{
				if (Counts[Chunk] > MaximumStaticMeshCollisionPayloadBytes / ElementSizes[Chunk])
					return Reject(EArchiveFailureCode::LimitExceeded, "DCOL count exceeds its byte limit.");
				Sizes[Chunk] = Counts[Chunk] * ElementSizes[Chunk];
				Offsets[Chunk] = AlignCollisionOffset(StoredSize);
				if (Offsets[Chunk] > MaximumStaticMeshCollisionPayloadBytes
					|| Sizes[Chunk] > MaximumStaticMeshCollisionPayloadBytes - Offsets[Chunk])
					return Reject(EArchiveFailureCode::LimitExceeded, "DCOL payload exceeds its byte limit.");
				StoredSize = Offsets[Chunk] + Sizes[Chunk];
				LogicalBytes += Sizes[Chunk];
			}
			FCollisionGeometryRef Validation;
			if (!MakeStaticMeshCollisionGeometry(*this, Validation, Error, ShouldCancel))
				return Reject(EArchiveFailureCode::InvalidData, Error);
			if (SourceMode == EBodySetupCollisionSourceMode::TriangleMeshFromLOD0)
			{
				std::map<uint32, uint32> OrdinalToTriangle;
				for (uint32 Triangle = 0; Triangle < SourceOrdinals.size(); ++Triangle)
					{ Control.Tick(); OrdinalToTriangle.emplace(SourceOrdinals[Triangle], Triangle); }
				OrderedIndices.reserve(Indices.size());
				OrderedOrdinals.reserve(LeafTriangles.size());
				for (uint32 Ordinal : LeafTriangles)
				{
					Control.Tick();
					const auto Found = OrdinalToTriangle.find(Ordinal);
					if (Found == OrdinalToTriangle.end())
						return Reject(EArchiveFailureCode::InvalidData, "DCOL leaf references an unknown source ordinal.");
					const size_t Begin = static_cast<size_t>(Found->second) * 3;
					OrderedIndices.insert(OrderedIndices.end(), Indices.begin() + Begin, Indices.begin() + Begin + 3);
					OrderedOrdinals.push_back(Ordinal);
				}
				if (OrderedIndices.size() != Indices.size() || OrderedOrdinals.size() != SourceOrdinals.size())
					return Reject(EArchiveFailureCode::InvalidData, "DCOL leaf ordering is incomplete.");
				WireIndices = &OrderedIndices;
				WireOrdinals = &OrderedOrdinals;
			}
			for (uint32 Chunk = 0; Chunk < 4; ++Chunk)
				Streams[Chunk] = std::make_unique<FCanonicalMemoryWriter>(Buffers[Chunk]);
			TransferStreams();
			Body.reserve(static_cast<size_t>(StoredSize - 64));
			FCanonicalMemoryWriter BodyAr(Body);
			TransferTable(BodyAr);
			for (uint32 Chunk = 0; Chunk < 4; ++Chunk)
			{
				while (BodyAr.Tell() + 64 < Offsets[Chunk]) { uint8 Zero = 0; BodyAr << Zero; }
				BodyAr.WriteBytes(Buffers[Chunk]);
			}
			if (BodyAr.HasError()) return Reject(BodyAr.GetFailure()->Code, BodyAr.GetError());
			Checksum = FXxHash64::HashBuffer(Body).HashValue;
		}

		Ar << Reserved0 << Schema << Builder << Platform << Header << ChunkCount << Alignment << Mode
			<< StoredSize << LogicalBytes << Checksum << Policy << Reserved;
		if (Ar.HasError()) return;
		if (Schema != StaticMeshCollisionPayloadSchemaVersion || Builder != StaticMeshCollisionBuilderVersion)
			return Reject(EArchiveFailureCode::UnsupportedVersion, "DCOL schema or builder version is unsupported.");
		if (Platform != static_cast<uint32>(TargetPlatform))
			return Reject(EArchiveFailureCode::UnsupportedTarget, "DCOL target platform does not match.");
		if (Reserved0 != 0 || Header != 64 || ChunkCount != 4
			|| Alignment != StaticMeshCollisionPayloadAlignment || Mode != 0 || Policy != 0 || Reserved != 0)
			return Reject(EArchiveFailureCode::InvalidData, "DCOL header layout is invalid.");
		if (StoredSize < 64 + 4 * StaticMeshCollisionPayloadChunkEntrySize
			|| StoredSize > MaximumStaticMeshCollisionPayloadBytes)
			return Reject(EArchiveFailureCode::LimitExceeded, "DCOL stored size exceeds its limit.");
		if (Ar.IsSaving())
		{
			Control.Check();
			Ar.WriteBytes(Body);
			return;
		}
		FByteView Region;
		if (!Ar.ReadRegion(StoredSize - 64, Region)) return;
		if (FXxHash64::HashBuffer(Region).HashValue != Checksum)
			return Reject(EArchiveFailureCode::InvalidData, "DCOL checksum does not match.");
		FCanonicalMemoryReader TableAr(Region.first(4 * StaticMeshCollisionPayloadChunkEntrySize));
		TransferTable(TableAr);
		if (TableAr.HasError()) return Reject(TableAr.GetFailure()->Code, TableAr.GetError());
		uint64 PreviousEnd = 64 + 4 * StaticMeshCollisionPayloadChunkEntrySize, Total = 0;
		for (uint32 Chunk = 0; Chunk < 4; ++Chunk)
		{
			if (Counts[Chunk] > std::numeric_limits<uint64>::max() / ElementSizes[Chunk])
				return Reject(EArchiveFailureCode::Overflow, "DCOL element byte count overflows.");
			if (Offsets[Chunk] % Alignment != 0 || Offsets[Chunk] < PreviousEnd || Offsets[Chunk] > StoredSize
				|| Sizes[Chunk] > StoredSize - Offsets[Chunk]
				|| Counts[Chunk] * ElementSizes[Chunk] != Sizes[Chunk])
				return Reject(EArchiveFailureCode::InvalidData, "DCOL chunk extent or count is invalid.");
			for (uint64 Offset = PreviousEnd; Offset < Offsets[Chunk]; ++Offset)
			{
				Control.Tick();
				if (Region[static_cast<size_t>(Offset - 64)] != std::byte{0})
					return Reject(EArchiveFailureCode::NonZeroPadding, "DCOL padding is nonzero.");
			}
			Streams[Chunk] = std::make_unique<FCanonicalMemoryReader>(Region.subspan(
				static_cast<size_t>(Offsets[Chunk] - 64), static_cast<size_t>(Sizes[Chunk])));
			PreviousEnd = Offsets[Chunk] + Sizes[Chunk];
			Total += Sizes[Chunk];
		}
		if (PreviousEnd != StoredSize) return Reject(EArchiveFailureCode::TrailingData, "DCOL has trailing bytes.");
		if (Counts[0] == 0 || Counts[0] > MaximumStaticMeshVerticesPerLOD || Counts[1] == 0
			|| Counts[1] % 3 != 0 || Counts[2] != Counts[1] / 3 || Counts[2] > 2'000'000 || Total != LogicalBytes)
			return Reject(EArchiveFailureCode::LimitExceeded, "DCOL logical counts are invalid.");
		const uint64 NativeBytes = Counts[0] * sizeof(FVector3f) + Counts[1] * sizeof(uint32)
			+ Counts[2] * sizeof(uint32) * (Counts[3] ? 2 : 1) + Counts[3] * sizeof(FCollisionGeometryNode);
		if (NativeBytes > MaximumStaticMeshCollisionPayloadBytes)
			return Reject(EArchiveFailureCode::LimitExceeded, "DCOL decoded allocation budget is exceeded.");
		SourceMode = Counts[3] == 0 ? EBodySetupCollisionSourceMode::ConvexHullFromLOD0
			: EBodySetupCollisionSourceMode::TriangleMeshFromLOD0;
		QueryPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		Control.Check();
		Positions.resize(static_cast<size_t>(Counts[0]));
		Indices.resize(static_cast<size_t>(Counts[1]));
		SourceOrdinals.resize(static_cast<size_t>(Counts[2]));
		Nodes.resize(static_cast<size_t>(Counts[3]));
		LeafTriangles.clear();
		TransferStreams();
		for (auto& Stream : Streams)
			if (!RequireArchiveEnd(*Stream)) return Reject(Stream->GetFailure()->Code, Stream->GetError());
		if (!Nodes.empty()) LeafTriangles = SourceOrdinals;
		FCollisionGeometryRef Validation;
		if (!MakeStaticMeshCollisionGeometry(*this, Validation, Error, ShouldCancel))
			return Reject(EArchiveFailureCode::InvalidData, Error);
	}
	catch (const FPayloadBuildCancelled&)
	{
		Ar.Fail(EArchiveFailureCode::InvalidData, "StaticMesh payload operation was cancelled.");
	}
}
