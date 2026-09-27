#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMeshPayloadValidation.h"

#include "Asset/ChunkedPayload.h"
#include "Asset/PayloadBuildControl.h"
#include "Serialization/Archive.h"
#include "Serialization/BinaryFormat.h"

namespace Durin
{
	namespace
	{
		auto ResolveMeshTarget(FArchive& Ar, EAssetPayloadTargetPlatform& TargetPlatform) -> bool
		{
			if (Ar.GetTarget().Platform != "Win64")
			{
				Ar.Fail(EArchiveFailureCode::UnsupportedTarget, "Static-mesh Archive target is missing or unsupported.");
				return false;
			}
			TargetPlatform = EAssetPayloadTargetPlatform::Win64;
			return true;
		}

		using AssetPrivate::FPayloadBuildControl;
		using AssetPrivate::FPayloadBuildCancelled;

		inline constexpr uint32 StaticMeshPayloadRequiredChunkCount = 6;
		inline constexpr uint32 StaticMeshPayloadFlagCompressed = 1;
		inline constexpr uint32 StaticMeshChunkCompressionMask = 0x0000ff00;
		inline constexpr uint32 StaticMeshChunkCompressionZstandard = 1;
		inline constexpr uint64 StaticMeshMaximumCompressionRatio = 64;

		using StaticMeshPrivate::IsValidBounds;
		using StaticMeshPrivate::ValidatePayload;

		auto SerializeBounds(FArchive& Ar, FBox& Bounds) -> void
		{
			std::array<float, 6> Values{};
			if (Ar.IsSaving())
				Values = {static_cast<float>(Bounds.Min.x), static_cast<float>(Bounds.Min.y),
					static_cast<float>(Bounds.Min.z), static_cast<float>(Bounds.Max.x),
					static_cast<float>(Bounds.Max.y), static_cast<float>(Bounds.Max.z)};
			for (float& Value : Values) Ar << Value;
			if (Ar.IsLoading() && !Ar.IsError())
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
			if (Metadata.IsError()) return;
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
				if (Metadata.IsError()) return;
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
			if (!ReadLittleEndianAt(Header, SizeOffset, Size))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, "Mesh payload stored size is outside its header.");
				return false;
			}
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

	auto FormatStaticMeshPayloadError(const FStaticMeshPayloadError& Error) -> std::string
	{
		std::string_view Reason;
		switch (Error.Code)
		{
		case EStaticMeshPayloadError::None: return {};
		case EStaticMeshPayloadError::Bounds: Reason = "bounds are invalid or not exactly representable as float32."; break;
		case EStaticMeshPayloadError::MaterialSlotCount: Reason = "material-slot count is outside the supported range."; break;
		case EStaticMeshPayloadError::LODCount: Reason = "LOD count is outside the supported range."; break;
		case EStaticMeshPayloadError::ScreenSize: Reason = "screen size must be finite and in [0, 1]."; break;
		case EStaticMeshPayloadError::ScreenSizeOrder: Reason = "LOD screen sizes must be strictly descending."; break;
		case EStaticMeshPayloadError::VertexCount: Reason = "has an invalid vertex count."; break;
		case EStaticMeshPayloadError::IndexCount: Reason = "has an invalid index count."; break;
		case EStaticMeshPayloadError::SectionCount: Reason = "has an invalid section count."; break;
		case EStaticMeshPayloadError::UVChannelCount: Reason = "has an invalid UV-channel count."; break;
		case EStaticMeshPayloadError::StoredSize: Reason = "exceeds the stored-object size limit."; break;
		case EStaticMeshPayloadError::VertexStreamCount: Reason = "vertex-stream counts do not match."; break;
		case EStaticMeshPayloadError::UVStreamCount:
			return std::format("Static-mesh payload LOD {} UV stream {} has {} values; expected {}.",
				Error.LODIndex.value_or(0), Error.Channel.value_or(0), Error.Actual, Error.Expected);
		case EStaticMeshPayloadError::ColorStreamCount: Reason = "color-stream count does not match its flags."; break;
		case EStaticMeshPayloadError::NonFiniteAttribute: Reason = "contains a non-finite vertex attribute."; break;
		case EStaticMeshPayloadError::NonFiniteUV: Reason = "contains a non-finite UV."; break;
		case EStaticMeshPayloadError::IndexRange: Reason = "contains an out-of-range index."; break;
		case EStaticMeshPayloadError::SectionCoverage: Reason = "sections do not exactly cover its index buffer."; break;
		case EStaticMeshPayloadError::SectionVertexRange: Reason = "section has an invalid vertex range."; break;
		case EStaticMeshPayloadError::SectionMaterialSlot: Reason = "section has an invalid material slot."; break;
		case EStaticMeshPayloadError::SectionBounds: Reason = "section bounds are invalid."; break;
		case EStaticMeshPayloadError::SectionVertexMismatch: Reason = "section vertex range does not match its indices."; break;
		case EStaticMeshPayloadError::IncompleteCoverage: Reason = "sections do not cover its complete index buffer."; break;
		case EStaticMeshPayloadError::FinalScreenSize: Reason = "lowest-detail LOD screen size must be exactly zero."; break;
		case EStaticMeshPayloadError::Cancelled: Reason = "operation was cancelled."; break;
		}
		return Error.LODIndex ? std::format("Static-mesh payload LOD {} {}", *Error.LODIndex, Reason)
			: std::format("Static-mesh payload {}", Reason);
	}

	auto MakeStaticMeshPayloadData(
		const FStaticMeshRenderData& RenderData,
		FStaticMeshPayloadData& OutPayload,
		const std::function<bool()>& ShouldCancel) -> std::expected<void, FStaticMeshPayloadError>
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
			const auto Positions = SourceLOD.VertexBuffers.PositionVertexBuffer
					.GetPositions();
			LOD.Positions.assign(Positions.begin(), Positions.end());
			Control.Check();
			const auto Normals = SourceLOD.VertexBuffers.StaticMeshVertexBuffer
					.TangentsVertexBuffer.GetNormals();
			LOD.Normals.assign(Normals.begin(), Normals.end());
			Control.Check();
			const auto Tangents = SourceLOD.VertexBuffers.StaticMeshVertexBuffer
					.TangentsVertexBuffer.GetTangents();
			LOD.Tangents.assign(Tangents.begin(), Tangents.end());
			Control.Check();
			const auto Indices = SourceLOD.IndexBuffer.GetIndices();
			LOD.Indices.assign(Indices.begin(), Indices.end());
			LOD.LocalBounds = SourceLOD.LocalBounds;
			LOD.ScreenSize = SourceLOD.ScreenSize;
			LOD.NumTexCoords = SourceLOD.NumTexCoords;
			if (LOD.NumTexCoords > MaxStaticMeshUVChannels)
				return std::unexpected(FStaticMeshPayloadError{.Code = EStaticMeshPayloadError::UVChannelCount, .LODIndex = Payload.LODs.size() - 1,
					.Actual = LOD.NumTexCoords, .Expected = MaxStaticMeshUVChannels});
			LOD.bHasVertexColors =
				SourceLOD.bHasColorVertexData;
			const auto& SourceTexCoords =
				SourceLOD.VertexBuffers.StaticMeshVertexBuffer
					.TexCoordVertexBuffer.GetTexCoords();
			for (uint32 Channel = 0; Channel < LOD.NumTexCoords; ++Channel)
			{
				Control.Tick();
				LOD.TexCoords[Channel].assign(SourceTexCoords[Channel].begin(), SourceTexCoords[Channel].end());
			}
			if (LOD.bHasVertexColors)
			{
				const auto Colors = SourceLOD.VertexBuffers.ColorVertexBuffer
						.GetColors();
				LOD.Colors.assign(Colors.begin(), Colors.end());
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
		if (const auto Result = ValidatePayload(Payload, Control); !Result) return Result;
		Control.Check();
		OutPayload = std::move(Payload);
		return {};
	}
	catch (const FPayloadBuildCancelled&)
	{
		return std::unexpected(FStaticMeshPayloadError{.Code = EStaticMeshPayloadError::Cancelled});
	}

	auto ValidateStaticMeshRenderData(const FStaticMeshRenderData& RenderData,
		const std::function<bool()>& ShouldCancel) -> std::expected<void, FStaticMeshPayloadError>
	try
	{
		struct FLODView
		{
			std::span<const FVector3f> Positions, Normals;
			std::span<const FVector4f> Tangents, Colors;
			std::span<const uint32> Indices;
			std::array<std::span<const FVector2f>, MaxStaticMeshUVChannels> TexCoords;
			std::span<const FStaticMeshSection> Sections;
			FBox LocalBounds;
			float ScreenSize;
			uint8 NumTexCoords;
			bool bHasVertexColors;
		};
		struct FView
		{
			FBox LocalBounds;
			size_t MaterialSlotCount;
			std::vector<FLODView> LODs;
		} View{RenderData.LocalBounds, RenderData.MaterialSlots.size(), {}};
		AssetPrivate::FPayloadBuildControl Control{ShouldCancel};
		Control.Check();
		if (RenderData.LODResources.size() > MaximumStaticMeshLODs)
			return std::unexpected(FStaticMeshPayloadError{.Code = EStaticMeshPayloadError::LODCount,
				.Actual = RenderData.LODResources.size(), .Expected = MaximumStaticMeshLODs});
		View.LODs.reserve(RenderData.LODResources.size());
		for (const auto& LOD : RenderData.LODResources)
		{
			const auto& Buffers = LOD.VertexBuffers;
			const auto& Frames = Buffers.StaticMeshVertexBuffer.TangentsVertexBuffer;
			auto UVs = Buffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.GetTexCoords();
			for (uint32 Channel = LOD.NumTexCoords; Channel < MaxStaticMeshUVChannels; ++Channel) UVs[Channel] = {};
			View.LODs.push_back({Buffers.PositionVertexBuffer.GetPositions(), Frames.GetNormals(), Frames.GetTangents(),
				LOD.bHasColorVertexData ? Buffers.ColorVertexBuffer.GetColors() : std::span<const FVector4f>{},
				LOD.IndexBuffer.GetIndices(), UVs, LOD.Sections, LOD.LocalBounds, LOD.ScreenSize, LOD.NumTexCoords, LOD.bHasColorVertexData});
		}
		auto Result = StaticMeshPrivate::ValidatePayload(View, Control);
		Control.Check();
		return Result;
	}
	catch (const AssetPrivate::FPayloadBuildCancelled&)
	{
		return std::unexpected(FStaticMeshPayloadError{.Code = EStaticMeshPayloadError::Cancelled});
	}

	auto MakeStaticMeshRenderData(
		const FStaticMeshPayloadData& Payload,
		std::unique_ptr<FStaticMeshRenderData>& OutRenderData,
		const std::function<bool()>& ShouldCancel) -> std::expected<void, FStaticMeshPayloadError>
	try
	{
		FPayloadBuildControl Control{ShouldCancel};
		Control.Check();
		if (const auto Result = ValidatePayload(Payload, Control); !Result) return Result;
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
		return {};
	}
	catch (const FPayloadBuildCancelled&)
	{
		return std::unexpected(FStaticMeshPayloadError{.Code = EStaticMeshPayloadError::Cancelled});
	}

	auto FStaticMeshPayloadData::Serialize(FArchive& Ar,
		const std::function<bool()>& ShouldCancel) -> void
	try
	{
		if (Ar.IsError()) return;
		FPayloadBuildControl Control{ShouldCancel};
		Control.Check();
		EAssetPayloadTargetPlatform TargetPlatform;
		if (!ResolveMeshTarget(Ar, TargetPlatform)) return;
		std::array<FByteBuffer, 6> Buffers;
		std::array<std::unique_ptr<FArchive>, 6> Owners;
		std::array<FArchive*, 6> Chunks;
		FDecodedChunkedPayload Container;
		if (Ar.IsSaving())
		{
			if (const auto Result = ValidatePayload(*this, Control); !Result)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, FormatStaticMeshPayloadError(Result.error()));
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
			if (Chunk->IsError())
			{
				Ar.Fail(Chunk->GetFailure()->Code, Chunk->GetFailure()->Message);
				return;
			}
		if (Ar.IsLoading())
		{
			if (const auto Result = ValidatePayload(*this, Control); !Result) Ar.Fail(EArchiveFailureCode::InvalidData, FormatStaticMeshPayloadError(Result.error()));
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

}
