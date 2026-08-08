#include "SkeletalMesh/SkeletalMesh.h"

#include "Math/Operations.h"

namespace Durin
{
	namespace
	{
		auto Fail(std::string* OutError, std::string Message) -> bool
		{
			if (OutError) *OutError = std::move(Message);
			return false;
		}

		auto IsFinite(const FBox& Bounds) -> bool
		{
			return Bounds.bIsValid && Math::IsFinite(Bounds.Min) && Math::IsFinite(Bounds.Max)
				&& Bounds.Min.x <= Bounds.Max.x && Bounds.Min.y <= Bounds.Max.y
				&& Bounds.Min.z <= Bounds.Max.z;
		}

		template<typename TValue>
		auto AllFinite(std::span<const TValue> Values) -> bool
		{
			return std::ranges::all_of(Values, [](const TValue& Value) {
				return Math::IsFinite(Value);
			});
		}

		auto IsFinite(const FMatrix4f& Matrix) -> bool
		{
			for (uint32 Column = 0; Column < 4; ++Column)
				if (!Math::IsFinite(Matrix[Column])) return false;
			return true;
		}

		auto AddPayloadBytes(uint64 Count, uint64 ElementSize, uint64& Total) -> bool
		{
			if (Count > MaximumSkeletalMeshPayloadBytes / ElementSize) return false;
			const uint64 Bytes = Count * ElementSize;
			if (Total > MaximumSkeletalMeshPayloadBytes - Bytes) return false;
			Total += Bytes;
			return true;
		}
	}

	auto FSkeletalMeshBounds::IsValid(std::string* OutError) const -> bool
	{
		if (!bIsValid || !Math::IsFinite(Minimum) || !Math::IsFinite(Maximum)
			|| Minimum.x > Maximum.x || Minimum.y > Maximum.y || Minimum.z > Maximum.z)
			return Fail(OutError, "Skeletal-mesh bounds are invalid.");
		if (OutError) OutError->clear();
		return true;
	}

	auto FSkeletalMeshBounds::ToBox() const -> FBox
	{
		if (!bIsValid) return {};
		return FBox(Minimum, Maximum);
	}

	auto FSkeletalMeshBounds::FromBox(const FBox& Box) -> FSkeletalMeshBounds
	{
		if (!Box.bIsValid) return {};
		return {.Minimum = Box.Min, .Maximum = Box.Max, .bIsValid = true};
	}

	auto ValidateSkeletalMeshPayload(
		const FSkeletalMeshPayloadData& Payload,
		const DSkeleton& Skeleton,
		uint32 MaterialSlotCount,
		std::string& OutError) -> bool
	{
		const size_t VertexCount = Payload.Positions.size();
		const size_t IndexCount = Payload.Indices.size();
		if (VertexCount == 0 || VertexCount > MaximumSkeletalMeshVertices)
			return Fail(&OutError, "Skeletal-mesh payload vertex count is outside the supported range.");
		if (IndexCount == 0 || IndexCount > MaximumSkeletalMeshIndices || IndexCount % 3 != 0)
			return Fail(&OutError, "Skeletal-mesh payload index count is invalid.");
		if (Payload.Sections.empty() || Payload.Sections.size() > MaximumSkeletalMeshSections)
			return Fail(&OutError, "Skeletal-mesh payload section count is outside the supported range.");
		if (MaterialSlotCount == 0 || MaterialSlotCount > MaximumSkeletalMeshMaterialSlots)
			return Fail(&OutError, "Skeletal-mesh payload material-slot count is outside the supported range.");
		if (Payload.Normals.size() != VertexCount || Payload.Tangents.size() != VertexCount
			|| Payload.Influences.size() != VertexCount)
			return Fail(&OutError, "Skeletal-mesh payload vertex stream counts do not match.");
		for (const auto& UVChannel : Payload.UVChannels)
			if (!UVChannel.empty() && UVChannel.size() != VertexCount)
				return Fail(&OutError, "Skeletal-mesh payload UV stream count does not match.");
		if (!Payload.Colors.empty() && Payload.Colors.size() != VertexCount)
			return Fail(&OutError, "Skeletal-mesh payload color stream count does not match.");
		if (!AllFinite<FVector3f>(Payload.Positions) || !AllFinite<FVector3f>(Payload.Normals)
			|| !AllFinite<FVector4f>(Payload.Tangents) || !AllFinite<FVector4f>(Payload.Colors))
			return Fail(&OutError, "Skeletal-mesh payload contains a non-finite vertex value.");
		for (const auto& UVChannel : Payload.UVChannels)
			if (!AllFinite<FVector2f>(UVChannel))
				return Fail(&OutError, "Skeletal-mesh payload contains a non-finite UV value.");
		if (!IsFinite(Payload.LocalBounds))
			return Fail(&OutError, "Skeletal-mesh payload local bounds are invalid.");
		if (std::ranges::any_of(Payload.Indices, [VertexCount](uint32 Index) { return Index >= VertexCount; }))
			return Fail(&OutError, "Skeletal-mesh payload contains an out-of-range vertex index.");
		uint64 PayloadBytes = 0;
		if (!AddPayloadBytes(Payload.Positions.size(), sizeof(FVector3f), PayloadBytes)
			|| !AddPayloadBytes(Payload.Normals.size(), sizeof(FVector3f), PayloadBytes)
			|| !AddPayloadBytes(Payload.Tangents.size(), sizeof(FVector4f), PayloadBytes)
			|| !AddPayloadBytes(Payload.Colors.size(), sizeof(FVector4f), PayloadBytes)
			|| !AddPayloadBytes(Payload.Indices.size(), sizeof(uint32), PayloadBytes)
			|| !AddPayloadBytes(Payload.Influences.size(), sizeof(FSkeletalMeshVertexInfluences), PayloadBytes)
			|| !AddPayloadBytes(Payload.Sections.size(), sizeof(FSkeletalMeshSection), PayloadBytes)
			|| !AddPayloadBytes(Payload.PaletteBoneIndices.size(), sizeof(uint16), PayloadBytes)
			|| !AddPayloadBytes(Payload.InverseBindMatrices.size(), sizeof(FMatrix4f), PayloadBytes))
			return Fail(&OutError, "Skeletal-mesh payload exceeds the supported byte limit.");
		for (const auto& UVChannel : Payload.UVChannels)
			if (!AddPayloadBytes(UVChannel.size(), sizeof(FVector2f), PayloadBytes))
				return Fail(&OutError, "Skeletal-mesh payload exceeds the supported byte limit.");

		if (Payload.PaletteBoneIndices.empty()
			|| Payload.PaletteBoneIndices.size() != Payload.InverseBindMatrices.size()
			|| Payload.PaletteBoneIndices.size() > Skeleton.GetBoneCount())
			return Fail(&OutError, "Skeletal-mesh palette and inverse-bind counts are invalid.");
		std::unordered_set<uint16> Palette;
		for (size_t PaletteIndex = 0; PaletteIndex < Payload.PaletteBoneIndices.size(); ++PaletteIndex)
		{
			const uint16 BoneIndex = Payload.PaletteBoneIndices[PaletteIndex];
			if (BoneIndex >= Skeleton.GetBoneCount() || !Palette.insert(BoneIndex).second)
				return Fail(&OutError, "Skeletal-mesh palette contains an invalid or duplicate bone index.");
			if (!IsFinite(Payload.InverseBindMatrices[PaletteIndex]))
				return Fail(&OutError, "Skeletal-mesh payload contains a non-finite inverse-bind matrix.");
		}

		for (const FSkeletalMeshVertexInfluences& Vertex : Payload.Influences)
		{
			if (Vertex.Count == 0 || Vertex.Count > MaximumSkeletalMeshInfluences)
				return Fail(&OutError, "Skeletal-mesh vertex influence count is invalid.");
			float Sum = 0.0f;
			std::unordered_set<uint16> VertexBones;
			for (uint8 InfluenceIndex = 0; InfluenceIndex < Vertex.Count; ++InfluenceIndex)
			{
				const uint16 BoneIndex = Vertex.BoneIndices[InfluenceIndex];
				const float Weight = Vertex.Weights[InfluenceIndex];
				if (!Palette.contains(BoneIndex) || !VertexBones.insert(BoneIndex).second
					|| !std::isfinite(Weight) || Weight <= 0.0f)
					return Fail(&OutError, "Skeletal-mesh vertex contains an invalid bone or weight.");
				if (InfluenceIndex > 0)
				{
					const float PreviousWeight = Vertex.Weights[InfluenceIndex - 1];
					const uint16 PreviousBone = Vertex.BoneIndices[InfluenceIndex - 1];
					if (Weight > PreviousWeight || (Weight == PreviousWeight && BoneIndex <= PreviousBone))
						return Fail(&OutError, "Skeletal-mesh influences are not in canonical order.");
				}
				Sum += Weight;
			}
			for (uint8 InfluenceIndex = Vertex.Count; InfluenceIndex < MaximumSkeletalMeshInfluences; ++InfluenceIndex)
				if (Vertex.BoneIndices[InfluenceIndex] != 0 || Vertex.Weights[InfluenceIndex] != 0.0f)
					return Fail(&OutError, "Skeletal-mesh unused influence slots must be zero.");
			if (std::abs(Sum - 1.0f) > 1.0e-5f)
				return Fail(&OutError, "Skeletal-mesh vertex weights do not sum to one.");
		}

		uint64 CoveredIndices = 0;
		for (const FSkeletalMeshSection& Section : Payload.Sections)
		{
			const uint64 End = static_cast<uint64>(Section.FirstIndex) + Section.IndexCount;
			if (Section.Name.IsNone() || Section.IndexCount == 0 || Section.IndexCount % 3 != 0
				|| Section.FirstIndex != CoveredIndices || End > IndexCount
				|| Section.MinVertexIndex > Section.MaxVertexIndex || Section.MaxVertexIndex >= VertexCount
				|| Section.MaterialSlotIndex >= MaterialSlotCount || !IsFinite(Section.LocalBounds))
				return Fail(&OutError, "Skeletal-mesh payload section is invalid or does not form a contiguous partition.");
			uint32 ActualMinimum = std::numeric_limits<uint32>::max();
			uint32 ActualMaximum = 0;
			for (uint64 IndexOffset = Section.FirstIndex; IndexOffset < End; ++IndexOffset)
			{
				ActualMinimum = std::min(ActualMinimum, Payload.Indices[static_cast<size_t>(IndexOffset)]);
				ActualMaximum = std::max(ActualMaximum, Payload.Indices[static_cast<size_t>(IndexOffset)]);
			}
			if (ActualMinimum != Section.MinVertexIndex || ActualMaximum != Section.MaxVertexIndex)
				return Fail(&OutError, "Skeletal-mesh section vertex range does not match its indices.");
			CoveredIndices = End;
		}
		if (CoveredIndices != IndexCount)
			return Fail(&OutError, "Skeletal-mesh sections do not cover the complete index buffer.");
		OutError.clear();
		return true;
	}

	DSkeletalMesh::DSkeletalMesh(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer) {}

	auto DSkeletalMesh::InitializeFromImportedData(
		FSkeletalMeshImportedData InData,
		std::string& OutError) -> bool
	{
		if (!InData.Skeleton || !InData.Payload)
			return Fail(&OutError, "Skeletal-mesh imported data requires a Skeleton and payload.");
		if (InData.SkeletonCompatibilityIdentity != InData.Skeleton->GetCompatibilityIdentity())
			return Fail(&OutError, "Skeletal-mesh imported data is incompatible with its Skeleton.");
		if (!InData.MeshNodeBindTransform.IsValid(&OutError))
		{
			OutError = std::format("Skeletal-mesh bind transform is invalid: {}", OutError);
			return false;
		}
		if (InData.MaterialSlots.empty() || InData.MaterialSlots.size() > MaximumSkeletalMeshMaterialSlots)
			return Fail(&OutError, "Skeletal-mesh material-slot count is outside the supported range.");
		std::unordered_set<FName> Names;
		std::unordered_set<uint32> SourceIndices;
		for (const FSkeletalMeshMaterialSlotDefinition& Slot : InData.MaterialSlots)
			if (Slot.Name.IsNone() || !Names.insert(Slot.Name).second
				|| !SourceIndices.insert(Slot.SourceMaterialIndex).second)
				return Fail(&OutError, "Skeletal-mesh material slots require unique non-None names and source indices.");
		if (!ValidateSkeletalMeshPayload(
			*InData.Payload, *InData.Skeleton,
			static_cast<uint32>(InData.MaterialSlots.size()), OutError)) return false;

		Skeleton = InData.Skeleton;
		SkeletonCompatibilityIdentity = std::move(InData.SkeletonCompatibilityIdentity);
		MeshNodeBindTransform = InData.MeshNodeBindTransform;
		MaterialSlots = std::move(InData.MaterialSlots);
		Summary = {
			.VertexCount = static_cast<uint32>(InData.Payload->Positions.size()),
			.IndexCount = static_cast<uint32>(InData.Payload->Indices.size()),
			.SectionCount = static_cast<uint32>(InData.Payload->Sections.size()),
			.LocalBounds = FSkeletalMeshBounds::FromBox(InData.Payload->LocalBounds)};
		CookedPayload = InData.CookedPayload;
		PayloadData = std::move(InData.Payload);
		MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DSkeletalMesh::Validate(std::string& OutError) const -> bool
	{
		if (!Skeleton)
			return Fail(&OutError, "SkeletalMesh has no Skeleton reference.");
		if (!Skeleton->Validate(OutError))
		{
			OutError = std::format("SkeletalMesh references an invalid Skeleton: {}", OutError);
			return false;
		}
		if (SkeletonCompatibilityIdentity != Skeleton->GetCompatibilityIdentity())
			return Fail(&OutError, "SkeletalMesh compatibility identity does not match its Skeleton.");
		if (!MeshNodeBindTransform.IsValid(&OutError))
		{
			OutError = std::format("SkeletalMesh bind transform is invalid: {}", OutError);
			return false;
		}
		if (MaterialSlots.empty() || MaterialSlots.size() > MaximumSkeletalMeshMaterialSlots)
			return Fail(&OutError, "SkeletalMesh material-slot count is outside the supported range.");
		std::unordered_set<FName> Names;
		std::unordered_set<uint32> SourceIndices;
		for (const FSkeletalMeshMaterialSlotDefinition& Slot : MaterialSlots)
			if (Slot.Name.IsNone() || !Names.insert(Slot.Name).second
				|| !SourceIndices.insert(Slot.SourceMaterialIndex).second)
				return Fail(&OutError, "SkeletalMesh material slots are not canonical and unique.");
		if (Summary.VertexCount == 0 || Summary.VertexCount > MaximumSkeletalMeshVertices
			|| Summary.IndexCount == 0 || Summary.IndexCount > MaximumSkeletalMeshIndices
			|| Summary.IndexCount % 3 != 0 || Summary.SectionCount == 0
			|| Summary.SectionCount > MaximumSkeletalMeshSections || !Summary.LocalBounds.IsValid(&OutError))
			return Fail(&OutError, "SkeletalMesh authored summary is invalid.");
		if (PayloadData)
		{
			if (!ValidateSkeletalMeshPayload(
				*PayloadData, *Skeleton, static_cast<uint32>(MaterialSlots.size()), OutError)) return false;
			if (PayloadData->Positions.size() != Summary.VertexCount
				|| PayloadData->Indices.size() != Summary.IndexCount
				|| PayloadData->Sections.size() != Summary.SectionCount
				|| FSkeletalMeshBounds::FromBox(PayloadData->LocalBounds) != Summary.LocalBounds)
				return Fail(&OutError, "SkeletalMesh payload does not match its authored summary.");
		}
		OutError.clear();
		return true;
	}

	auto DSkeletalMesh::PostLoad(std::string& OutError) -> bool
	{
		if (!Super::PostLoad(OutError)) return false;
		if (Validate(OutError)) return true;
		OutError = std::format("{}: {}", GetName(), OutError);
		return false;
	}

	auto DSkeletalMesh::PrepareImportedStateExchange(
		DSkeletalMesh& Candidate,
		std::string& OutError) -> std::unique_ptr<FSkeletalMeshImportedStateExchange>
	{
		if (&Candidate == this)
			return Fail(&OutError, "SkeletalMesh imported-state exchange requires distinct assets."), nullptr;
		if (!Validate(OutError))
		{
			OutError = std::format("Target SkeletalMesh is invalid: {}", OutError);
			return nullptr;
		}
		if (!Candidate.Validate(OutError))
		{
			OutError = std::format("Candidate SkeletalMesh is invalid: {}", OutError);
			return nullptr;
		}
		OutError.clear();
		return std::unique_ptr<FSkeletalMeshImportedStateExchange>(
			new FSkeletalMeshImportedStateExchange(*this, Candidate));
	}

	FSkeletalMeshImportedStateExchange::FSkeletalMeshImportedStateExchange(
		DSkeletalMesh& InTarget,
		DSkeletalMesh& InCandidate)
		: Target(&InTarget), Candidate(&InCandidate) {}

	FSkeletalMeshImportedStateExchange::~FSkeletalMeshImportedStateExchange() = default;

	auto FSkeletalMeshImportedStateExchange::Swap() noexcept -> void
	{
		check(Target && Candidate && Target != Candidate);
		std::swap(Target->Skeleton, Candidate->Skeleton);
		std::swap(Target->SkeletonCompatibilityIdentity, Candidate->SkeletonCompatibilityIdentity);
		std::swap(Target->MeshNodeBindTransform, Candidate->MeshNodeBindTransform);
		std::swap(Target->MaterialSlots, Candidate->MaterialSlots);
		std::swap(Target->Summary, Candidate->Summary);
		std::swap(Target->CookedPayload, Candidate->CookedPayload);
		std::swap(Target->PayloadData, Candidate->PayloadData);
		Target->MarkPackageDirty();
	}

	auto FSkeletalMeshImportedStateExchange::Commit() noexcept -> void
	{
		if (bCommitted) return;
		Swap();
		bCommitted = true;
	}

	auto FSkeletalMeshImportedStateExchange::Reverse() noexcept -> void
	{
		if (!bCommitted) return;
		Swap();
		bCommitted = false;
	}

	auto FSkeletalMeshImportedStateExchange::Finalize() noexcept -> void
	{
		Target = nullptr;
		Candidate = nullptr;
	}
}
