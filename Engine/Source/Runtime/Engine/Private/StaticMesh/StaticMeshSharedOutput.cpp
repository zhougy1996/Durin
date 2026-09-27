#include "StaticMeshSharedOutput.h"
#if DURIN_WITH_EDITOR
#include "StaticMeshPayloadValidation.h"
#include "Asset/CookedAsset.h"
#include "Serialization/BinaryFormat.h"
#include <bit>
#include <cstring>

namespace Durin::StaticMeshPrivate
{
	using namespace DerivedData;
	namespace
	{
		// Current supported targets use these exact portable little-endian layouts.
		static_assert(std::endian::native == std::endian::little);
		static_assert(sizeof(float) == 4 && sizeof(double) == 8 && sizeof(uint32) == 4);
		static_assert(std::numeric_limits<float>::is_iec559 && std::numeric_limits<double>::is_iec559);
		static_assert(sizeof(FVector2f) == 8 && offsetof(FVector2f, y) == 4);
		static_assert(sizeof(FVector3f) == 12 && offsetof(FVector3f, y) == 4 && offsetof(FVector3f, z) == 8);
		static_assert(sizeof(FVector4f) == 16 && offsetof(FVector4f, y) == 4 && offsetof(FVector4f, z) == 8 && offsetof(FVector4f, w) == 12);
		constexpr uint64 SectionBytes = 5 * 4 + 6 * 8;
		auto WriteBounds(FBinaryWriter& Writer, const FBox& Box) -> void
		{
			for (uint32 Axis = 0; Axis < 3; ++Axis) Writer.WriteDouble(Box.Min[Axis]);
			for (uint32 Axis = 0; Axis < 3; ++Axis) Writer.WriteDouble(Box.Max[Axis]);
		}
		auto ReadBounds(FBinaryReader& Reader, FBox& Box) -> bool
		{
			Box.bIsValid = true;
			for (uint32 Axis = 0; Axis < 3; ++Axis) if (!Reader.ReadDouble(Box.Min[Axis])) return false;
			for (uint32 Axis = 0; Axis < 3; ++Axis) if (!Reader.ReadDouble(Box.Max[Axis])) return false;
			return IsValidBounds(Box);
		}
		template<typename T> struct FStream
		{
			FSharedByteBuffer Block;
			FByteView Bytes;
			std::span<const T> Native;
			auto size() const -> size_t { return Bytes.size() / sizeof(T); }
			auto empty() const -> bool { return size() == 0; }
			auto operator[](size_t Index) const -> T
			{
				if (!Native.empty()) return Native[Index];
				T Value;
				std::memcpy(&Value, Bytes.data() + Index * sizeof(T), sizeof(T));
				return Value;
			}
			auto Freeze(const std::function<bool()>& ShouldCancel) const -> FSharedByteBuffer
			{
				if (Block.GetNativeView<T>()) return Block;
				std::vector<T> Values(size());
				// Serialized bytes never get cast to objects. The vector establishes lifetime/alignment.
				for (size_t Offset = 0; Offset < Values.size(); Offset += 4096)
				{
					if (ShouldCancel && ShouldCancel()) throw AssetPrivate::FPayloadBuildCancelled{};
					const auto Count = std::min<size_t>(4096, Values.size() - Offset);
					std::memcpy(Values.data() + Offset, Bytes.data() + Offset * sizeof(T), Count * sizeof(T));
				}
				return FSharedByteBuffer::TakeNative(std::move(Values));
			}
		};
		struct FSections
		{
			FSharedByteBuffer Block;
			auto size() const -> size_t { return Block.GetSize() / SectionBytes; }
			auto empty() const -> bool { return size() == 0; }
			auto operator[](size_t Index) const -> FStaticMeshPayloadSection
			{
				FBinaryReader Reader(Block.GetBytes().subspan(Index * SectionBytes, SectionBytes));
				FStaticMeshPayloadSection Result;
				Reader.ReadU32(Result.FirstIndex); Reader.ReadU32(Result.IndexCount);
				Reader.ReadU32(Result.MinVertexIndex); Reader.ReadU32(Result.MaxVertexIndex); Reader.ReadU32(Result.MaterialSlotIndex);
				// Bounds validity is checked by the common validator, including the valid bit.
				Result.LocalBounds.bIsValid = ReadBounds(Reader, Result.LocalBounds);
				return Result;
			}
		};
		struct FLOD
		{
			FStream<FVector3f> Positions, Normals;
			FStream<FVector4f> Tangents, Colors;
			FStream<uint32> Indices;
			std::array<FStream<FVector2f>, MaxStaticMeshUVChannels> TexCoords;
			FSections Sections;
			FBox LocalBounds;
			float ScreenSize = 0;
			uint32 NumTexCoords = 0;
			bool bHasVertexColors = false;
		};
		struct FLayout { FBox LocalBounds; uint32 MaterialSlotCount = 0; std::vector<FLOD> LODs; };
		template<typename T>
		auto ReadStream(const FBuildOutput& Output, const std::string& Id, uint32 Count, FStream<T>& Stream) -> bool
		{
			const auto* Value = Output.FindValue(Id);
			if (!Value || Value->Data.GetSize() != uint64(Count) * sizeof(T)) return false;
			Stream.Block = Value->Data; Stream.Bytes = Stream.Block.GetBytes();
			if (auto Native = Stream.Block.template GetNativeView<T>()) Stream.Native = *Native;
			return true;
		}
		auto ReadLayout(const FBuildOutput& Output, const std::function<bool()>& ShouldCancel) -> std::expected<FLayout, std::string>
		{
			AssetPrivate::FPayloadBuildControl Control{ShouldCancel}; Control.Check();
			if (Output.GetSchema() != "StaticMesh.RenderOutput" || Output.GetSchemaVersion() != 1
				|| !Output.CheckLimits({.MaximumTotalBytes = MaximumStaticMeshPayloadBytes}))
				return std::unexpected("StaticMesh output schema or size is invalid.");
			FBinaryReader Reader(Output.GetMetadata().GetBytes(), {.MaximumTotalBytes = 64 + MaximumStaticMeshLODs * 72});
			uint32 Platform = 0, Profile = 0, Count = 0;
			FLayout Layout;
			if (!Reader.ReadU32(Platform) || !Reader.ReadU32(Profile) || !Reader.ReadU32(Layout.MaterialSlotCount)
				|| !Reader.ReadU32(Count) || !Count || Count > MaximumStaticMeshLODs
				|| Platform != uint32(ECookTargetPlatform::Win64) || Profile != uint32(ECookTargetProfile::Game)
				|| !ReadBounds(Reader, Layout.LocalBounds)) return std::unexpected("StaticMesh output header is invalid.");
			Layout.LODs.resize(Count);
			size_t ValueCount = 0;
			for (uint32 Index = 0; Index < Count; ++Index)
			{
				Control.Check(); auto& LOD = Layout.LODs[Index];
				uint32 Vertices = 0, Indices = 0, Colors = 0, Sections = 0;
				if (!Reader.ReadFloat(LOD.ScreenSize) || !ReadBounds(Reader, LOD.LocalBounds)
					|| !Reader.ReadU32(Vertices) || !Reader.ReadU32(Indices) || !Reader.ReadU32(LOD.NumTexCoords)
					|| !Reader.ReadU32(Colors) || !Reader.ReadU32(Sections)
					|| Vertices > MaximumStaticMeshVerticesPerLOD || Indices > MaximumStaticMeshIndicesPerLOD || Indices % 3 != 0
					|| LOD.NumTexCoords > MaxStaticMeshUVChannels || Colors > 1 || Sections > MaximumStaticMeshSectionsPerLOD)
					return std::unexpected("StaticMesh output LOD descriptor is invalid.");
				LOD.bHasVertexColors = Colors != 0;
				const auto Prefix = std::format("LOD/{}/", Index);
				if (!ReadStream(Output, Prefix + "Positions", Vertices, LOD.Positions)
					|| !ReadStream(Output, Prefix + "Normals", Vertices, LOD.Normals)
					|| !ReadStream(Output, Prefix + "Tangents", Vertices, LOD.Tangents)
					|| !ReadStream(Output, Prefix + "Indices", Indices, LOD.Indices))
					return std::unexpected("StaticMesh output stream size is invalid.");
				ValueCount += 5 + LOD.NumTexCoords + Colors;
				for (uint32 Channel = 0; Channel < LOD.NumTexCoords; ++Channel)
					if (!ReadStream(Output, Prefix + std::format("UV/{}", Channel), Vertices, LOD.TexCoords[Channel]))
						return std::unexpected("StaticMesh output UV stream is invalid.");
				if (Colors && !ReadStream(Output, Prefix + "Colors", Vertices, LOD.Colors))
					return std::unexpected("StaticMesh output color stream is invalid.");
				const auto* SectionValue = Output.FindValue(Prefix + "Sections");
				if (!SectionValue || SectionValue->Data.GetSize() != uint64(Sections) * SectionBytes)
					return std::unexpected("StaticMesh output section table is invalid.");
				LOD.Sections.Block = SectionValue->Data;
				for (size_t SectionIndex = 0; SectionIndex < LOD.Sections.size(); ++SectionIndex)
				{
					Control.Tick();
					if (LOD.Sections[SectionIndex].IndexCount % 3 != 0)
						return std::unexpected("StaticMesh output section is not a triangle list.");
				}
			}
			if (!Reader.IsAtEnd() || Output.GetValues().size() != ValueCount)
				return std::unexpected("StaticMesh output contains extra metadata or streams.");
			if (auto Valid = ValidatePayload(Layout, Control); !Valid) return std::unexpected(FormatStaticMeshPayloadError(Valid.error()));
			Control.Check();
			return Layout;
		}
	}

	auto ValidateSharedOutput(const FBuildOutput& Output, const std::function<bool()>& ShouldCancel) -> std::expected<void, std::string>
	try { auto Layout = ReadLayout(Output, ShouldCancel); if (!Layout) return std::unexpected(std::move(Layout.error())); return {}; }
	catch (const AssetPrivate::FPayloadBuildCancelled&) { return std::unexpected("StaticMesh output validation was cancelled."); }

	auto MakeSharedOutput(FStaticMeshRenderBuildProduct Product, uint32 MaterialSlotCount,
		const std::function<bool()>& ShouldCancel) -> std::expected<FBuildOutput, std::string>
	{
		if (!IsValidBounds(Product.LocalBounds) || Product.LODs.empty() || Product.LODs.size() > MaximumStaticMeshLODs)
			return std::unexpected("StaticMesh output LOD count is invalid.");
		FBuildOutputData Data{.Schema = "StaticMesh.RenderOutput", .SchemaVersion = 1};
		FBinaryWriter Metadata({.MaximumTotalBytes = 64 + MaximumStaticMeshLODs * 72});
		Metadata.WriteU32(uint32(ECookTargetPlatform::Win64)); Metadata.WriteU32(uint32(ECookTargetProfile::Game));
		Metadata.WriteU32(MaterialSlotCount); Metadata.WriteU32(uint32(Product.LODs.size())); WriteBounds(Metadata, Product.LocalBounds);
		for (uint32 Index = 0; Index < Product.LODs.size(); ++Index)
		{
			if (ShouldCancel && ShouldCancel()) return std::unexpected("StaticMesh output construction was cancelled.");
			auto& LOD = Product.LODs[Index];
			if (!IsValidBounds(LOD.LocalBounds) || LOD.Positions.size() > MaximumStaticMeshVerticesPerLOD || LOD.Indices.size() > MaximumStaticMeshIndicesPerLOD
				|| LOD.Sections.size() > MaximumStaticMeshSectionsPerLOD || LOD.NumTexCoords > MaxStaticMeshUVChannels)
				return std::unexpected("StaticMesh recipe counts exceed their bounds.");
			Metadata.WriteFloat(LOD.ScreenSize); WriteBounds(Metadata, LOD.LocalBounds);
			Metadata.WriteU32(uint32(LOD.Positions.size())); Metadata.WriteU32(uint32(LOD.Indices.size()));
			Metadata.WriteU32(LOD.NumTexCoords); Metadata.WriteU32(LOD.bHasColorVertexData); Metadata.WriteU32(uint32(LOD.Sections.size()));
			const auto Prefix = std::format("LOD/{}/", Index);
			Data.Values.push_back({Prefix + "Positions", FSharedByteBuffer::TakeNative(std::move(LOD.Positions))});
			Data.Values.push_back({Prefix + "Normals", FSharedByteBuffer::TakeNative(std::move(LOD.Normals))});
			Data.Values.push_back({Prefix + "Tangents", FSharedByteBuffer::TakeNative(std::move(LOD.Tangents))});
			Data.Values.push_back({Prefix + "Indices", FSharedByteBuffer::TakeNative(std::move(LOD.Indices))});
			for (uint32 Channel = 0; Channel < LOD.NumTexCoords; ++Channel)
				Data.Values.push_back({Prefix + std::format("UV/{}", Channel), FSharedByteBuffer::TakeNative(std::move(LOD.TexCoords[Channel]))});
			if (LOD.bHasColorVertexData) Data.Values.push_back({Prefix + "Colors", FSharedByteBuffer::TakeNative(std::move(LOD.Colors))});
			FBinaryWriter Sections({.MaximumTotalBytes = MaximumStaticMeshSectionsPerLOD * SectionBytes});
			for (const auto& Section : LOD.Sections)
			{
				if (!IsValidBounds(Section.LocalBounds)) return std::unexpected("StaticMesh recipe section bounds are invalid.");
				if (ShouldCancel && ShouldCancel()) return std::unexpected("StaticMesh output construction was cancelled.");
				Sections.WriteU32(Section.FirstIndex); Sections.WriteU32(Section.IndexCount);
				Sections.WriteU32(Section.MinVertexIndex); Sections.WriteU32(Section.MaxVertexIndex); Sections.WriteU32(Section.MaterialSlotIndex);
				WriteBounds(Sections, Section.LocalBounds);
			}
			if (Sections.HasError()) return std::unexpected("StaticMesh section metadata exceeds its bound.");
			Data.Values.push_back({Prefix + "Sections", FSharedByteBuffer::Take(Sections.TakeBytes())});
		}
		if (Metadata.HasError()) return std::unexpected("StaticMesh output metadata exceeds its bound.");
		Data.Metadata = FSharedByteBuffer::Take(Metadata.TakeBytes());
		auto Output = FBuildOutput::TryCreate(std::move(Data), {.MaximumTotalBytes = MaximumStaticMeshPayloadBytes});
		if (!Output) return Output;
		if (auto Valid = ValidateSharedOutput(*Output, ShouldCancel); !Valid) return std::unexpected(std::move(Valid.error()));
		return Output;
	}

	auto AssembleSharedOutput(const FBuildOutput& Output, const std::function<bool()>& ShouldCancel)
		-> std::expected<std::unique_ptr<FStaticMeshRenderData>, std::string>
	try
	{
		auto Layout = ReadLayout(Output, ShouldCancel);
		if (!Layout) return std::unexpected(std::move(Layout.error()));
		auto Result = std::make_unique<FStaticMeshRenderData>();
		Result->LocalBounds = Layout->LocalBounds; Result->MaterialSlots.resize(Layout->MaterialSlotCount);
		for (const auto& Source : Layout->LODs)
		{
			if (ShouldCancel && ShouldCancel()) throw AssetPrivate::FPayloadBuildCancelled{};
			auto& LOD = Result->LODResources.emplace_back(); auto& Buffers = LOD.VertexBuffers;
			Buffers.PositionVertexBuffer.SetSharedPositions(Source.Positions.Freeze(ShouldCancel));
			Buffers.StaticMeshVertexBuffer.TangentsVertexBuffer.SetSharedNormals(Source.Normals.Freeze(ShouldCancel));
			Buffers.StaticMeshVertexBuffer.TangentsVertexBuffer.SetSharedTangents(Source.Tangents.Freeze(ShouldCancel));
			LOD.IndexBuffer.SetSharedIndices(Source.Indices.Freeze(ShouldCancel));
			for (uint32 Channel = 0; Channel < Source.NumTexCoords; ++Channel)
				Buffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.SetSharedTexCoord(Channel, Source.TexCoords[Channel].Freeze(ShouldCancel));
			if (Source.bHasVertexColors) Buffers.ColorVertexBuffer.SetSharedColors(Source.Colors.Freeze(ShouldCancel));
			LOD.LocalBounds = Source.LocalBounds; LOD.ScreenSize = Source.ScreenSize;
			LOD.NumTexCoords = uint8(Source.NumTexCoords); LOD.bHasColorVertexData = Source.bHasVertexColors;
			for (size_t Index = 0; Index < Source.Sections.size(); ++Index)
			{
				if ((Index % 256) == 0 && ShouldCancel && ShouldCancel()) throw AssetPrivate::FPayloadBuildCancelled{};
				const auto Section = Source.Sections[Index];
				LOD.Sections.push_back({.FirstIndex = Section.FirstIndex, .IndexCount = Section.IndexCount,
					.MinVertexIndex = Section.MinVertexIndex, .MaxVertexIndex = Section.MaxVertexIndex,
					.MaterialSlotIndex = Section.MaterialSlotIndex, .LocalBounds = Section.LocalBounds});
			}
			Buffers.Finalize(LOD.NumTexCoords, LOD.bHasColorVertexData);
		}
		if (ShouldCancel && ShouldCancel()) throw AssetPrivate::FPayloadBuildCancelled{};
		return Result;
	}
	catch (const AssetPrivate::FPayloadBuildCancelled&) { return std::unexpected("StaticMesh output assembly was cancelled."); }
}
#endif
