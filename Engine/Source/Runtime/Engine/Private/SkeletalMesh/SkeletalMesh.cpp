#include "SkeletalMesh/SkeletalMesh.h"

#include "AssetSystem.h"
#include "CoreGlobals.h"
#include "DObject/Property.h"
#include "DynamicRHI.h"
#include "Math/Operations.h"
#include "RenderingThread.h"
#include "SkeletalMesh/SkeletalDerivedData.h"
#include "SkeletalMesh/SkeletalMeshResources.h"

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

		auto Contains(const FBox& Bounds, const FVector3f& Position) -> bool
		{
			return Position.x >= Bounds.Min.x && Position.x <= Bounds.Max.x
				&& Position.y >= Bounds.Min.y && Position.y <= Bounds.Max.y
				&& Position.z >= Bounds.Min.z && Position.z <= Bounds.Max.z;
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
		if (std::ranges::any_of(Payload.Positions, [&Payload](const FVector3f& Position) {
			return !Contains(Payload.LocalBounds, Position);
		}))
			return Fail(&OutError, "Skeletal-mesh payload local bounds do not contain its geometry.");
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
				const uint32 VertexIndex = Payload.Indices[static_cast<size_t>(IndexOffset)];
				ActualMinimum = std::min(ActualMinimum, VertexIndex);
				ActualMaximum = std::max(ActualMaximum, VertexIndex);
				if (!Contains(Section.LocalBounds, Payload.Positions[VertexIndex]))
					return Fail(&OutError, "Skeletal-mesh section bounds do not contain its indexed geometry.");
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

	DSkeletalMesh::~DSkeletalMesh() = default;

	auto DSkeletalMesh::GetRenderData() const -> const FSkeletalMeshRenderData*
	{
		return RenderData.get();
	}

	auto DSkeletalMesh::GetMaterialSlot(uint32 SlotIndex) const
		-> const FSkeletalMeshMaterialSlotDefinition*
	{
		return SlotIndex < MaterialSlots.size() ? &MaterialSlots[SlotIndex] : nullptr;
	}

	auto DSkeletalMesh::FindMaterialSlot(FName Name) const
		-> const FSkeletalMeshMaterialSlotDefinition*
	{
		const auto It = std::ranges::find(MaterialSlots, Name,
			&FSkeletalMeshMaterialSlotDefinition::Name);
		return It != MaterialSlots.end() ? &*It : nullptr;
	}

	auto DSkeletalMesh::BuildRenderData(std::string& OutError) -> bool
	{
		if (!Skeleton || !PayloadData)
			return Fail(&OutError, "SkeletalMesh render data requires a Skeleton and payload.");
		std::unique_ptr<FSkeletalMeshRenderData> Candidate;
		if (!BuildSkeletalMeshRenderData(*PayloadData, *Skeleton, MeshNodeBindTransform,
			MaterialSlots, Candidate, OutError)) return false;
		if (RenderData && RenderData->GetNumInitializedResources() != 0)
			return Fail(&OutError,
				"Initialized SkeletalMesh render data must be replaced through imported-state exchange.");
		RenderData = std::move(Candidate);
		RenderResourceState.store(ERenderResourceState::Uninitialized, std::memory_order_release);
		RenderResourceRevision.fetch_add(1, std::memory_order_acq_rel);
		OutError.clear();
		return true;
	}

	auto DSkeletalMesh::GetRenderResourceStatus() const
		-> FSkeletalMeshRenderResourceStatus
	{
		ESkeletalMeshRenderResourceReadiness Readiness =
			ESkeletalMeshRenderResourceReadiness::Unavailable;
		switch (RenderResourceState.load(std::memory_order_acquire))
		{
		case ERenderResourceState::InitializationQueued:
			Readiness = ESkeletalMeshRenderResourceReadiness::Queued; break;
		case ERenderResourceState::Ready:
			Readiness = ESkeletalMeshRenderResourceReadiness::Ready; break;
		case ERenderResourceState::Failed:
			Readiness = ESkeletalMeshRenderResourceReadiness::Failed; break;
		case ERenderResourceState::Uninitialized:
		case ERenderResourceState::ReleaseQueued:
		case ERenderResourceState::Released: break;
		}
		return {.Readiness = Readiness,
			.Revision = RenderResourceRevision.load(std::memory_order_acquire)};
	}

	auto DSkeletalMesh::InitResources() -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!RenderData || GDynamicRHI == nullptr) return;
		ERenderResourceState Expected = ERenderResourceState::Uninitialized;
		if (!RenderResourceState.compare_exchange_strong(
			Expected, ERenderResourceState::InitializationQueued, std::memory_order_acq_rel))
		{
			Expected = ERenderResourceState::Failed;
			if (!RenderResourceState.compare_exchange_strong(
				Expected, ERenderResourceState::InitializationQueued, std::memory_order_acq_rel)) return;
		}
		RenderResourceRevision.fetch_add(1, std::memory_order_acq_rel);
#if DURIN_BUILD_DEBUG
		RenderData->SetResourceDebugOwner(GetPackage()
			? FName(GetPackage()->GetPackagePath())
			: FName(std::format("<transient DSkeletalMesh:{}>", GetName())));
#endif
		FSkeletalMeshRenderData* Data = RenderData.get();
		ENQUEUE_RENDER_COMMAND(InitSkeletalMeshResources)(
			[this, Data](FRHICommandListImmediate& CommandList) {
				RenderResourceState.store(Data->InitResources(CommandList)
					? ERenderResourceState::Ready : ERenderResourceState::Failed,
					std::memory_order_release);
				RenderResourceRevision.fetch_add(1, std::memory_order_acq_rel);
			});
	}

	auto DSkeletalMesh::ReleaseResources() -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		const ERenderResourceState State = RenderResourceState.load(std::memory_order_acquire);
		if (State == ERenderResourceState::Released || State == ERenderResourceState::ReleaseQueued)
			return;
		if (!RenderData || State == ERenderResourceState::Uninitialized)
		{
			RenderResourceState.store(ERenderResourceState::Released, std::memory_order_release);
			RenderResourceRevision.fetch_add(1, std::memory_order_acq_rel);
			return;
		}
		RenderResourceState.store(ERenderResourceState::ReleaseQueued, std::memory_order_release);
		RenderResourceRevision.fetch_add(1, std::memory_order_acq_rel);
		FSkeletalMeshRenderData* Data = RenderData.get();
		ENQUEUE_RENDER_COMMAND(ReleaseSkeletalMeshResources)(
			[this, Data](FRHICommandListImmediate&) {
				Data->ReleaseResources();
				RenderResourceState.store(ERenderResourceState::Released, std::memory_order_release);
				RenderResourceRevision.fetch_add(1, std::memory_order_acq_rel);
			});
	}

	auto DSkeletalMesh::InitializeFromImportedData(
		FSkeletalMeshImportedData InData,
		std::string& OutError) -> bool
	{
		if (!InData.Skeleton || !InData.Payload)
			return Fail(&OutError, "Skeletal-mesh imported data requires a Skeleton and payload.");
		const DSkeleton* ValidationSkeleton = InData.ValidationSkeleton
			? InData.ValidationSkeleton : InData.Skeleton;
		if (InData.SkeletonCompatibilityIdentity != ValidationSkeleton->GetCompatibilityIdentity())
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
			*InData.Payload, *ValidationSkeleton,
			static_cast<uint32>(InData.MaterialSlots.size()), OutError)) return false;

		std::unique_ptr<FSkeletalMeshRenderData> RenderDataCandidate;
		if (!BuildSkeletalMeshRenderData(*InData.Payload, *ValidationSkeleton,
			InData.MeshNodeBindTransform, InData.MaterialSlots,
			RenderDataCandidate, OutError)) return false;
		if (RenderData && RenderData->GetNumInitializedResources() != 0)
			return Fail(&OutError,
				"Initialized SkeletalMesh state must be replaced through imported-state exchange.");

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
		DerivedDataKey = std::move(InData.DerivedDataKey);
		PayloadData = std::move(InData.Payload);
		RenderData = std::move(RenderDataCandidate);
		RenderResourceState.store(ERenderResourceState::Uninitialized, std::memory_order_release);
		bLoadedFromDerivedDataCache = false;
		PayloadStorageDiagnostic.clear();
		if (!DerivedDataKey.empty())
		{
			std::vector<uint8> Bytes;
			std::string CacheError;
			if (EncodeSkeletalMeshPayload(
					*PayloadData, *ValidationSkeleton,
					static_cast<uint32>(MaterialSlots.size()),
					ESkeletalPayloadTargetPlatform::Win64,
					ESkeletalPayloadTargetProfile::Game,
					Bytes, CacheError)
				&& StoreSkeletalMeshDerivedData(DerivedDataKey, Bytes, CacheError))
				PayloadStorageDiagnostic = std::format(
					"Stored SkeletalMesh DDC key {}.", DerivedDataKey);
			else
				PayloadStorageDiagnostic = std::format(
					"SkeletalMesh DDC write failed for key {}: {}",
					DerivedDataKey, CacheError);
		}
		MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DSkeletalMesh::Validate(std::string& OutError) const -> bool
	{
		if (!Skeleton)
			return Fail(&OutError, "SkeletalMesh has no Skeleton reference.");
		return ValidateAgainstSkeleton(*Skeleton, OutError);
	}

	auto DSkeletalMesh::ValidateAgainstSkeleton(
		const DSkeleton& ProspectiveSkeleton,
		std::string& OutError) const -> bool
	{
		if (!ProspectiveSkeleton.Validate(OutError))
		{
			OutError = std::format("SkeletalMesh references an invalid Skeleton: {}", OutError);
			return false;
		}
		if (!Skeleton || SkeletonCompatibilityIdentity != ProspectiveSkeleton.GetCompatibilityIdentity())
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
		const bool bRequireSourceIndices =
			Asset::GetPackageLoadContext().Mode != Asset::EPackageLoadMode::CookedRuntime;
		for (const FSkeletalMeshMaterialSlotDefinition& Slot : MaterialSlots)
			if (Slot.Name.IsNone() || !Names.insert(Slot.Name).second
				|| (bRequireSourceIndices
					&& !SourceIndices.insert(Slot.SourceMaterialIndex).second))
				return Fail(&OutError, "SkeletalMesh material slots are not canonical and unique.");
		if (Summary.VertexCount == 0 || Summary.VertexCount > MaximumSkeletalMeshVertices
			|| Summary.IndexCount == 0 || Summary.IndexCount > MaximumSkeletalMeshIndices
			|| Summary.IndexCount % 3 != 0 || Summary.SectionCount == 0
			|| Summary.SectionCount > MaximumSkeletalMeshSections || !Summary.LocalBounds.IsValid(&OutError))
			return Fail(&OutError, "SkeletalMesh authored summary is invalid.");
		if (PayloadData)
		{
			if (!ValidateSkeletalMeshPayload(
				*PayloadData, ProspectiveSkeleton, static_cast<uint32>(MaterialSlots.size()), OutError)) return false;
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
		if (!Validate(OutError))
		{
			OutError = std::format("{}: {}", GetName(), OutError);
			return false;
		}
		if (Asset::IsAssetMigrationLoad()) return true;
		if (!PayloadData)
		{
			if (Asset::GetPackageLoadContext().Mode == Asset::EPackageLoadMode::CookedRuntime)
			{
				if (!LoadCookedPayload(OutError)) return false;
			}
			else if (!DerivedDataKey.empty() && !LoadDerivedDataPayload(OutError))
			{
				if (!IsSkeletalDerivedDataRepairLoadActive()) return false;
				ReportMissingSkeletalDerivedDataAsset(this);
				OutError.clear();
			}
		}
		return !PayloadData || BuildRenderData(OutError);
	}

	auto DSkeletalMesh::LoadDerivedDataPayload(std::string& OutError) -> bool
	{
		std::vector<uint8> Bytes;
		std::string CacheMessage;
		if (!LoadSkeletalMeshDerivedData(DerivedDataKey, Bytes, CacheMessage))
		{
			PayloadStorageDiagnostic = std::format(
				"SkeletalMesh DDC miss for key {}: {}", DerivedDataKey, CacheMessage);
			return Fail(&OutError, PayloadStorageDiagnostic);
		}
		FSkeletalMeshPayloadData Candidate;
		const FPayloadDecodeResult Decoded = DecodeSkeletalMeshPayload(
			Bytes, *Skeleton, static_cast<uint32>(MaterialSlots.size()),
			ESkeletalPayloadTargetPlatform::Win64,
			ESkeletalPayloadTargetProfile::Game, Candidate);
		if (!Decoded)
		{
			PayloadStorageDiagnostic = std::format(
				"SkeletalMesh DDC object {} is invalid: {}", DerivedDataKey, Decoded.Message);
			return Fail(&OutError, PayloadStorageDiagnostic);
		}
		if (Candidate.Positions.size() != Summary.VertexCount
			|| Candidate.Indices.size() != Summary.IndexCount
			|| Candidate.Sections.size() != Summary.SectionCount
			|| FSkeletalMeshBounds::FromBox(Candidate.LocalBounds) != Summary.LocalBounds)
			return Fail(&OutError, "SkeletalMesh DDC payload does not match authored summary.");
		PayloadData = std::make_shared<const FSkeletalMeshPayloadData>(std::move(Candidate));
		bLoadedFromDerivedDataCache = true;
		PayloadStorageDiagnostic = std::format(
			"Loaded SkeletalMesh DDC key {}.", DerivedDataKey);
		OutError.clear();
		return true;
	}

	auto DSkeletalMesh::LoadCookedPayload(std::string& OutError) -> bool
	{
		auto FailCooked = [&](std::string Message) {
			PayloadStorageDiagnostic = std::format(
				"Cooked SkeletalMesh '{}': {}", GetObjectPath(), Message);
			OutError = PayloadStorageDiagnostic;
			return false;
		};
		if (CookedPayload.PayloadId != SkeletalMeshPrimaryCookedPayloadId
			|| CookedPayload.LocationKind
				!= static_cast<uint32>(Asset::ECookedPayloadLocationKind::PackageCompanion)
			|| CookedPayload.PayloadSchemaVersion != SkeletalMeshPayloadSchemaVersion
			|| CookedPayload.TargetPlatform
				!= static_cast<uint32>(Asset::ECookTargetPlatform::Win64)
			|| CookedPayload.TargetProfile
				!= static_cast<uint32>(Asset::ECookTargetProfile::Game)
			|| CookedPayload.CompressionMethod
				!= static_cast<uint32>(Asset::ECookedPayloadCompression::None))
			return FailCooked("required DSKM descriptor is missing or incompatible.");

		const Asset::FPackageLoadContext& Context = Asset::GetPackageLoadContext();
		std::filesystem::path PackagePath;
		std::filesystem::path CompanionPath;
		if (!GetPackage()
			|| !Asset::ResolveCookedPackagePath(
				Context.CookRoot, GetPackage()->GetPackagePath(), PackagePath, &OutError)
			|| !Asset::ResolveCookedCompanionPath(
				Context.CookRoot, PackagePath, CompanionPath, &OutError))
			return FailCooked(OutError.empty()
				? "package companion path could not be resolved." : OutError);
		Asset::FCookedBulkContainer Container;
		if (!Asset::LoadCookedBulkFile(
			CompanionPath, Asset::ECookTargetPlatform::Win64,
			Asset::ECookTargetProfile::Game, Container, &OutError))
			return FailCooked(OutError);
		std::span<const uint8> Bytes;
		if (!Asset::ResolveCookedPayload(Container, CookedPayload, Bytes, &OutError))
			return FailCooked(OutError);
		FSkeletalMeshPayloadData Candidate;
		const FPayloadDecodeResult Decoded = DecodeSkeletalMeshPayload(
			Bytes, *Skeleton, static_cast<uint32>(MaterialSlots.size()),
			ESkeletalPayloadTargetPlatform::Win64,
			ESkeletalPayloadTargetProfile::Game, Candidate);
		if (!Decoded) return FailCooked(Decoded.Message);
		if (Candidate.Positions.size() != Summary.VertexCount
			|| Candidate.Indices.size() != Summary.IndexCount
			|| Candidate.Sections.size() != Summary.SectionCount
			|| FSkeletalMeshBounds::FromBox(Candidate.LocalBounds) != Summary.LocalBounds)
			return FailCooked("payload does not match authored summary.");
		PayloadData = std::make_shared<const FSkeletalMeshPayloadData>(std::move(Candidate));
		DerivedDataKey.clear();
		bLoadedFromDerivedDataCache = false;
		PayloadStorageDiagnostic = std::format(
			"Loaded cooked SkeletalMesh payload for '{}'.", GetObjectPath());
		OutError.clear();
		return true;
	}

	auto DSkeletalMesh::AddToCook(
		Asset::FCookContext& Context,
		std::string_view VirtualPackagePath,
		std::string& OutError,
		bool bRetainDiagnosticEditorMetadata) -> bool
	{
		if (Context.GetTargetPlatform() != Asset::ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != Asset::ECookTargetProfile::Game)
			return Fail(&OutError, std::format(
				"SkeletalMesh '{}' supports only the Win64 game cook target.", GetObjectPath()));
		if (!PayloadData && !PostLoad(OutError)) return false;
		if (!PayloadData) return Fail(&OutError, "SkeletalMesh has no CPU payload to cook.");
		std::vector<uint8> PayloadBytes;
		if (!EncodeSkeletalMeshPayload(
			*PayloadData, *Skeleton, static_cast<uint32>(MaterialSlots.size()),
			ESkeletalPayloadTargetPlatform::Win64,
			ESkeletalPayloadTargetProfile::Game, PayloadBytes, OutError))
			return false;
		Asset::FCookedBulkPayload BulkPayload{
			.PayloadId = SkeletalMeshPrimaryCookedPayloadId,
			.Flags = 1,
			.PayloadSchemaVersion = SkeletalMeshPayloadSchemaVersion,
			.Compression = Asset::ECookedPayloadCompression::None,
			.Alignment = SkeletalPayloadAlignment,
			.Bytes = std::move(PayloadBytes)};
		return Context.AddPackage(
			std::string(VirtualPackagePath), {std::move(BulkPayload)},
			[this, bRetainDiagnosticEditorMetadata](
				std::span<const Asset::FCookedPayloadDescriptor> Descriptors,
				std::vector<uint8>& OutPackageBytes,
				std::string* Error) {
				if (Descriptors.size() != 1
					|| Descriptors.front().PayloadId != SkeletalMeshPrimaryCookedPayloadId)
				{
					if (Error) *Error = "SkeletalMesh cook did not produce its required descriptor.";
					return false;
				}
				const Asset::FCookedPayloadDescriptor SavedDescriptor = CookedPayload;
				const std::string SavedKey = DerivedDataKey;
				const std::vector<FSkeletalMeshMaterialSlotDefinition> SavedSlots = MaterialSlots;
				CookedPayload = Descriptors.front();
				if (!bRetainDiagnosticEditorMetadata)
				{
					DerivedDataKey.clear();
					for (FSkeletalMeshMaterialSlotDefinition& Slot : MaterialSlots)
					{
						Slot.SourceName.clear();
						Slot.SourceMaterialIndex = 0;
					}
				}
				Asset::FAssetPackageSerializationOptions Options;
				if (!bRetainDiagnosticEditorMetadata)
					Options.PropertyFilter = [this](const DObject* Object, const FProperty* Property) {
						return Object != this || Property->NamePrivate != FName("DerivedDataKey");
					};
				const Asset::FAssetResult Serialized = Asset::SerializeAssetPackageBytes(
					GetPackage(), OutPackageBytes, Options);
				CookedPayload = SavedDescriptor;
				DerivedDataKey = SavedKey;
				MaterialSlots = SavedSlots;
				if (!Serialized)
				{
					if (Error) *Error = Serialized.Message;
					return false;
				}
				return true;
			}, &OutError);
	}

	auto DSkeletalMesh::PrepareImportedStateExchange(
		DSkeletalMesh& Candidate,
		std::string& OutError) -> std::unique_ptr<FSkeletalMeshImportedStateExchange>
	{
		if (!Candidate.GetSkeleton())
			return Fail(&OutError, "Candidate SkeletalMesh has no Skeleton reference."), nullptr;
		return PrepareImportedStateExchange(Candidate, *Candidate.GetSkeleton(), OutError);
	}

	auto DSkeletalMesh::PrepareImportedStateExchange(
		DSkeletalMesh& Candidate,
		const DSkeleton& ProspectiveSkeleton,
		std::string& OutError) -> std::unique_ptr<FSkeletalMeshImportedStateExchange>
	{
		if (&Candidate == this)
			return Fail(&OutError, "SkeletalMesh imported-state exchange requires distinct assets."), nullptr;
		if (!Validate(OutError))
		{
			OutError = std::format("Target SkeletalMesh is invalid: {}", OutError);
			return nullptr;
		}
		if (!Candidate.ValidateAgainstSkeleton(ProspectiveSkeleton, OutError))
		{
			OutError = std::format("Candidate SkeletalMesh is invalid: {}", OutError);
			return nullptr;
		}
		if (!RenderData && PayloadData && !BuildRenderData(OutError)) return nullptr;
		if (!Candidate.RenderData && Candidate.PayloadData
			&& !Candidate.BuildRenderData(OutError)) return nullptr;
		if (GDynamicRHI != nullptr && Candidate.RenderData
			&& Candidate.RenderResourceState.load(std::memory_order_acquire)
				!= ERenderResourceState::Ready)
		{
			Candidate.RenderResourceState.store(
				ERenderResourceState::InitializationQueued, std::memory_order_release);
			FSkeletalMeshRenderData* CandidateData = Candidate.RenderData.get();
			ENQUEUE_RENDER_COMMAND(InitSkeletalMeshExchangeCandidate)(
				[&Candidate, CandidateData](FRHICommandListImmediate& CommandList) {
					Candidate.RenderResourceState.store(
						CandidateData->InitResources(CommandList)
							? ERenderResourceState::Ready : ERenderResourceState::Failed,
						std::memory_order_release);
				});
			FRenderCommandFence CandidateFence;
			CandidateFence.BeginFence();
			CandidateFence.Wait();
			if (Candidate.RenderResourceState.load(std::memory_order_acquire)
				!= ERenderResourceState::Ready)
			{
				OutError = "SkeletalMesh replacement render-resource initialization failed.";
				return nullptr;
			}
		}
		FRenderCommandFence PriorResourceCommands;
		PriorResourceCommands.BeginFence();
		PriorResourceCommands.Wait();
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
		std::swap(Target->DerivedDataKey, Candidate->DerivedDataKey);
		std::swap(Target->PayloadData, Candidate->PayloadData);
		std::swap(Target->RenderData, Candidate->RenderData);
		const DSkeletalMesh::ERenderResourceState TargetState =
			Target->RenderResourceState.load(std::memory_order_acquire);
		const DSkeletalMesh::ERenderResourceState CandidateState =
			Candidate->RenderResourceState.load(std::memory_order_acquire);
		Target->RenderResourceState.store(CandidateState, std::memory_order_release);
		Candidate->RenderResourceState.store(TargetState, std::memory_order_release);
		Target->RenderResourceRevision.fetch_add(1, std::memory_order_acq_rel);
		Candidate->RenderResourceRevision.fetch_add(1, std::memory_order_acq_rel);
		std::swap(Target->bLoadedFromDerivedDataCache, Candidate->bLoadedFromDerivedDataCache);
		std::swap(Target->PayloadStorageDiagnostic, Candidate->PayloadStorageDiagnostic);
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

	auto DSkeletalMesh::BeginDestroy() -> void
	{
		const ERenderResourceState State = RenderResourceState.load(std::memory_order_acquire);
		const bool bHasQueuedWork = State != ERenderResourceState::Uninitialized
			&& State != ERenderResourceState::Released;
		ReleaseResources();
		if (bHasQueuedWork) ReleaseResourcesFence.BeginFence();
		Super::BeginDestroy();
	}

	auto DSkeletalMesh::IsReadyForFinishDestroy() -> bool
	{
		return ReleaseResourcesFence.IsFenceComplete() && Super::IsReadyForFinishDestroy();
	}

	auto DSkeletalMesh::FinishDestroy() -> void
	{
		check(ReleaseResourcesFence.IsFenceComplete());
		check(RenderResourceState.load(std::memory_order_acquire)
			== ERenderResourceState::Released);
		check(!RenderData || RenderData->GetNumInitializedResources() == 0);
		RenderData.reset();
		Super::FinishDestroy();
	}
}
