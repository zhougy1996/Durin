#pragma once
#include "StaticMesh/StaticMeshDerivedData.h"
#include "Asset/PayloadBuildControl.h"

namespace Durin::StaticMeshPrivate
{
	inline constexpr uint32 StaticMeshPayloadRequiredChunkCount = 6;
	inline auto IsFinite(const FVector2f& Value) -> bool
	{
		return std::isfinite(Value.x) && std::isfinite(Value.y);
	}

	inline auto IsFinite(const FVector3f& Value) -> bool
	{
		return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
	}

	inline auto IsFinite(const FVector4f& Value) -> bool
	{
		return std::isfinite(Value.x) && std::isfinite(Value.y)
			&& std::isfinite(Value.z) && std::isfinite(Value.w);
	}

	inline auto IsValidBounds(const FBox& Bounds) -> bool
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

	template<typename TPayload>
	auto ValidatePayload(const TPayload& Payload, AssetPrivate::FPayloadBuildControl& Control) -> std::expected<void, FStaticMeshPayloadError>
	{
		FStaticMeshPayloadError Error;
		const auto Reject = [&](EStaticMeshPayloadError Code, uint64 Actual = 0, uint64 Expected = 0) {
			Error.Code = Code;
			Error.Actual = Actual;
			Error.Expected = Expected;
			return std::unexpected(Error);
		};
		Error.Bounds = Payload.LocalBounds;
		if (!IsValidBounds(Payload.LocalBounds)) return Reject(EStaticMeshPayloadError::Bounds);
		if (Payload.MaterialSlotCount == 0 || Payload.MaterialSlotCount > MaximumMeshMaterialSlots)
			return Reject(EStaticMeshPayloadError::MaterialSlotCount, Payload.MaterialSlotCount, MaximumMeshMaterialSlots);
		if (Payload.LODs.empty() || Payload.LODs.size() > MaximumStaticMeshLODs)
			return Reject(EStaticMeshPayloadError::LODCount, Payload.LODs.size(), MaximumStaticMeshLODs);

		uint64 EncodedSizeUpperBound = StaticMeshPayloadHeaderSize
			+ StaticMeshPayloadRequiredChunkCount * StaticMeshPayloadChunkEntrySize
			+ StaticMeshPayloadRequiredChunkCount * (StaticMeshPayloadAlignment - 1)
			+ 24ull + 4ull + 4ull + static_cast<uint64>(Payload.LODs.size()) * 44ull;
		for (size_t LODIndex = 0; LODIndex < Payload.LODs.size(); ++LODIndex)
		{
			Control.Tick();
			const auto& LOD = Payload.LODs[LODIndex];
			Error = {.LODIndex = LODIndex, .ScreenSize = LOD.ScreenSize,
				.PreviousScreenSize = LODIndex ? Payload.LODs[LODIndex - 1].ScreenSize : 0.0f,
				.Bounds = LOD.LocalBounds};
			if (!std::isfinite(LOD.ScreenSize) || LOD.ScreenSize < 0.0f || LOD.ScreenSize > 1.0f
				|| (LOD.ScreenSize == 0.0f && std::signbit(LOD.ScreenSize)))
				return Reject(EStaticMeshPayloadError::ScreenSize);
			if (LODIndex > 0 && LOD.ScreenSize >= Payload.LODs[LODIndex - 1].ScreenSize)
				return Reject(EStaticMeshPayloadError::ScreenSizeOrder);
			const size_t VertexCount = LOD.Positions.size();
			const size_t IndexCount = LOD.Indices.size();
			if (VertexCount == 0 || VertexCount > MaximumStaticMeshVerticesPerLOD)
				return Reject(EStaticMeshPayloadError::VertexCount, VertexCount, MaximumStaticMeshVerticesPerLOD);
			if (IndexCount == 0 || IndexCount > MaximumStaticMeshIndicesPerLOD)
				return Reject(EStaticMeshPayloadError::IndexCount, IndexCount, MaximumStaticMeshIndicesPerLOD);
			if (LOD.Sections.empty() || LOD.Sections.size() > MaximumStaticMeshSectionsPerLOD)
				return Reject(EStaticMeshPayloadError::SectionCount, LOD.Sections.size(), MaximumStaticMeshSectionsPerLOD);
			if (LOD.NumTexCoords > MaxStaticMeshUVChannels)
				return Reject(EStaticMeshPayloadError::UVChannelCount, LOD.NumTexCoords, MaxStaticMeshUVChannels);
			const uint64 LODPayloadBytes = 4ull + static_cast<uint64>(LOD.Sections.size()) * 44ull
				+ static_cast<uint64>(VertexCount) * (40ull + static_cast<uint64>(LOD.NumTexCoords) * 8ull
					+ (LOD.bHasVertexColors ? 16ull : 0ull)) + static_cast<uint64>(IndexCount) * 4ull;
			if (LODPayloadBytes > MaximumStaticMeshPayloadBytes - EncodedSizeUpperBound)
				return Reject(EStaticMeshPayloadError::StoredSize, EncodedSizeUpperBound + LODPayloadBytes, MaximumStaticMeshPayloadBytes);
			EncodedSizeUpperBound += LODPayloadBytes;
			if (!IsValidBounds(LOD.LocalBounds)) return Reject(EStaticMeshPayloadError::Bounds);
			if (LOD.Normals.size() != VertexCount || LOD.Tangents.size() != VertexCount)
			{
				Error.Stream = LOD.Normals.size() != VertexCount ? EStaticMeshPayloadStream::Normal : EStaticMeshPayloadStream::Tangent;
				return Reject(EStaticMeshPayloadError::VertexStreamCount,
					LOD.Normals.size() != VertexCount ? LOD.Normals.size() : LOD.Tangents.size(), VertexCount);
			}
			for (uint32 Channel = 0; Channel < MaxStaticMeshUVChannels; ++Channel)
			{
				Control.Tick();
				const size_t ExpectedCount = Channel < LOD.NumTexCoords ? VertexCount : 0;
				if (LOD.TexCoords[Channel].size() != ExpectedCount)
				{
					Error.Channel = Channel;
					return Reject(EStaticMeshPayloadError::UVStreamCount, LOD.TexCoords[Channel].size(), ExpectedCount);
				}
			}
			if (LOD.Colors.size() != (LOD.bHasVertexColors ? VertexCount : 0))
				return Reject(EStaticMeshPayloadError::ColorStreamCount, LOD.Colors.size(), LOD.bHasVertexColors ? VertexCount : 0);
			const auto CheckStream = [&](const auto& Values, EStaticMeshPayloadStream Stream) {
				for (size_t Index = 0; Index < Values.size(); ++Index)
				{
					Control.Tick();
					if (IsFinite(Values[Index])) continue;
					Error.Stream = Stream;
					Error.ElementIndex = Index;
					const auto& Value = Values[Index];
					Error.Value = FVector4(Value.x, Value.y, 0, 0);
					if constexpr (requires { Value.z; }) Error.Value.z = Value.z;
					if constexpr (requires { Value.w; }) Error.Value.w = Value.w;
					return false;
				}
				return true;
			};
			if (!CheckStream(LOD.Positions, EStaticMeshPayloadStream::Position)
				|| !CheckStream(LOD.Normals, EStaticMeshPayloadStream::Normal)
				|| !CheckStream(LOD.Tangents, EStaticMeshPayloadStream::Tangent)
				|| !CheckStream(LOD.Colors, EStaticMeshPayloadStream::Color))
				return Reject(EStaticMeshPayloadError::NonFiniteAttribute);
			for (uint32 Channel = 0; Channel < LOD.NumTexCoords; ++Channel)
			{
				Control.Tick();
				if (!CheckStream(LOD.TexCoords[Channel], EStaticMeshPayloadStream::UV))
				{
					Error.Channel = Channel;
					return Reject(EStaticMeshPayloadError::NonFiniteUV);
				}
			}
			for (size_t Index = 0; Index < IndexCount; ++Index)
			{
				Control.Tick();
				if (LOD.Indices[Index] < VertexCount) continue;
				Error.ElementIndex = Index;
				Error.Stream = EStaticMeshPayloadStream::Index;
				return Reject(EStaticMeshPayloadError::IndexRange, LOD.Indices[Index], VertexCount);
			}
			uint64 CoveredIndices = 0;
			for (size_t SectionIndex = 0; SectionIndex < LOD.Sections.size(); ++SectionIndex)
			{
				Control.Tick();
				const auto& Section = LOD.Sections[SectionIndex];
				Error.SectionIndex = SectionIndex;
				Error.Section = FStaticMeshPayloadSection{.FirstIndex = Section.FirstIndex, .IndexCount = Section.IndexCount,
					.MinVertexIndex = Section.MinVertexIndex, .MaxVertexIndex = Section.MaxVertexIndex,
					.MaterialSlotIndex = Section.MaterialSlotIndex, .LocalBounds = Section.LocalBounds};
				const uint64 SectionEnd = static_cast<uint64>(Section.FirstIndex) + Section.IndexCount;
				if (Section.IndexCount == 0 || Section.FirstIndex != CoveredIndices || SectionEnd > IndexCount)
				{
					Error.AdditionalActual = SectionEnd;
					Error.AdditionalExpected = IndexCount;
					return Reject(EStaticMeshPayloadError::SectionCoverage, Section.FirstIndex, CoveredIndices);
				}
				if (Section.MinVertexIndex > Section.MaxVertexIndex || Section.MaxVertexIndex >= VertexCount)
					return Reject(EStaticMeshPayloadError::SectionVertexRange, Section.MaxVertexIndex, VertexCount);
				if (Section.MaterialSlotIndex >= Payload.MaterialSlotCount)
					return Reject(EStaticMeshPayloadError::SectionMaterialSlot, Section.MaterialSlotIndex, Payload.MaterialSlotCount);
				if (!IsValidBounds(Section.LocalBounds)) return Reject(EStaticMeshPayloadError::SectionBounds);
				uint32 ActualMinimum = std::numeric_limits<uint32>::max();
				uint32 ActualMaximum = 0;
				for (uint64 IndexOffset = Section.FirstIndex; IndexOffset < SectionEnd; ++IndexOffset)
				{
					Control.Tick();
					ActualMinimum = std::min(ActualMinimum, LOD.Indices[static_cast<size_t>(IndexOffset)]);
					ActualMaximum = std::max(ActualMaximum, LOD.Indices[static_cast<size_t>(IndexOffset)]);
				}
				if (ActualMinimum != Section.MinVertexIndex || ActualMaximum != Section.MaxVertexIndex)
				{
					Error.AdditionalActual = ActualMaximum;
					Error.AdditionalExpected = Section.MaxVertexIndex;
					return Reject(EStaticMeshPayloadError::SectionVertexMismatch, ActualMinimum, Section.MinVertexIndex);
				}
				CoveredIndices = SectionEnd;
			}
			Error.SectionIndex.reset();
			Error.Section.reset();
			if (CoveredIndices != IndexCount)
				return Reject(EStaticMeshPayloadError::IncompleteCoverage, CoveredIndices, IndexCount);
		}
		if (Payload.LODs.back().ScreenSize != 0.0f) return Reject(EStaticMeshPayloadError::FinalScreenSize);
		return {};
	}

}
