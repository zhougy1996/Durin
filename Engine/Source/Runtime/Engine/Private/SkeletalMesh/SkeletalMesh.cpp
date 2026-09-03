#include "SkeletalMesh/SkeletalMesh.h"

#include "Asset/CookedMeshProducts.h"
#include "Asset/CookedMeshLoadManager.h"

#include "DObject/Package.h"

#include "Asset/AssetCook.h"
#include "CoreGlobals.h"
#include "DObject/Property.h"
#include "DynamicRHI.h"
#include "Hash/XxHash.h"
#include "Math/Operations.h"
#include "RenderingThread.h"
#include "Serialization/Archive.h"
#include "SkeletalMesh/SkeletalAssetBuild.h"
#include "SkeletalMesh/SkeletalDerivedData.h"
#include "SkeletalMesh/SkeletalMeshResources.h"
#include "SkeletalMesh/SkeletalMeshRenderStateRecreateContext.h"

namespace Durin
{
	const FGuid SkeletalMeshImportedDataPayloadId{
		0xe1246757, 0xfac3498d, 0xa4ec5161, 0x5d956391};

	namespace
	{
		struct FSkeletalMeshManagerProduct final
			: ICookedMeshDetachedProduct
		{
			FSkeletalMeshCookedProduct Product;
		};

		auto BuildSkeletalCookedMetadataIdentity(const DSkeletalMesh& Mesh) -> uint64
		{
			FXxHash64Builder Builder;
			const FBulkDataMetadata Metadata =
				Mesh.GetCookedPlatformData().GetMetadata();
			Builder.UpdateValue(Metadata.LogicalSize);
			Builder.UpdateValue(Metadata.Range.SegmentOffset);
			Builder.UpdateValue(Metadata.Range.StoredSize);
			Builder.UpdateValue(Metadata.Range.StorageFlags);
			Builder.UpdateValue(Metadata.Range.Alignment);
			Builder.UpdateValue(reinterpret_cast<uintptr_t>(Metadata.Range.Resource.get()));
			Builder.Update(Mesh.GetSkeletonCompatibilityIdentity());
			const FSkeletonTransform& Transform = Mesh.GetMeshNodeBindTransform();
			Builder.UpdateValue(Transform.Row0);
			Builder.UpdateValue(Transform.Row1);
			Builder.UpdateValue(Transform.Row2);
			Builder.UpdateValue(Transform.Row3);
			const FSkeletalMeshSummary& Summary = Mesh.GetSummary();
			Builder.UpdateValue(Summary.VertexCount);
			Builder.UpdateValue(Summary.IndexCount);
			Builder.UpdateValue(Summary.SectionCount);
			Builder.UpdateValue(Summary.LocalBounds.Minimum);
			Builder.UpdateValue(Summary.LocalBounds.Maximum);
			Builder.UpdateValue(Summary.LocalBounds.bIsValid);
			for (const FMeshMaterialSlotDefinition& Slot : Mesh.GetMaterialSlots())
			{
				Builder.Update(Slot.Name.ToString());
				Builder.UpdateValue(Slot.SourceMaterialIndex);
			}
			return Builder.Finalize().HashValue;
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
			return Fail("Skeletal-mesh bounds are invalid.", OutError);
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
		uint32 SkeletonBoneCount,
		uint32 MaterialSlotCount,
		std::string& OutError) -> bool
	{
		const size_t VertexCount = Payload.Positions.size();
		const size_t IndexCount = Payload.Indices.size();
		if (VertexCount == 0 || VertexCount > MaximumSkeletalMeshVertices)
			return Fail("Skeletal-mesh payload vertex count is outside the supported range.", &OutError);
		if (IndexCount == 0 || IndexCount > MaximumSkeletalMeshIndices || IndexCount % 3 != 0)
			return Fail("Skeletal-mesh payload index count is invalid.", &OutError);
		if (Payload.Sections.empty() || Payload.Sections.size() > MaximumSkeletalMeshSections)
			return Fail("Skeletal-mesh payload section count is outside the supported range.", &OutError);
		if (MaterialSlotCount == 0 || MaterialSlotCount > MaximumMeshMaterialSlots)
			return Fail("Skeletal-mesh payload material-slot count is outside the supported range.", &OutError);
		if (Payload.Normals.size() != VertexCount || Payload.Tangents.size() != VertexCount
			|| Payload.Influences.size() != VertexCount)
			return Fail("Skeletal-mesh payload vertex stream counts do not match.", &OutError);
		for (const auto& UVChannel : Payload.UVChannels)
			if (!UVChannel.empty() && UVChannel.size() != VertexCount)
				return Fail("Skeletal-mesh payload UV stream count does not match.", &OutError);
		if (!Payload.Colors.empty() && Payload.Colors.size() != VertexCount)
			return Fail("Skeletal-mesh payload color stream count does not match.", &OutError);
		if (!AllFinite<FVector3f>(Payload.Positions) || !AllFinite<FVector3f>(Payload.Normals)
			|| !AllFinite<FVector4f>(Payload.Tangents) || !AllFinite<FVector4f>(Payload.Colors))
			return Fail("Skeletal-mesh payload contains a non-finite vertex value.", &OutError);
		for (const auto& UVChannel : Payload.UVChannels)
			if (!AllFinite<FVector2f>(UVChannel))
				return Fail("Skeletal-mesh payload contains a non-finite UV value.", &OutError);
		if (!IsFinite(Payload.LocalBounds))
			return Fail("Skeletal-mesh payload local bounds are invalid.", &OutError);
		if (std::ranges::any_of(Payload.Positions, [&Payload](const FVector3f& Position) {
			return !Contains(Payload.LocalBounds, Position);
		}))
			return Fail("Skeletal-mesh payload local bounds do not contain its geometry.", &OutError);
		if (std::ranges::any_of(Payload.Indices, [VertexCount](uint32 Index) { return Index >= VertexCount; }))
			return Fail("Skeletal-mesh payload contains an out-of-range vertex index.", &OutError);
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
			return Fail("Skeletal-mesh payload exceeds the supported byte limit.", &OutError);
		for (const auto& UVChannel : Payload.UVChannels)
			if (!AddPayloadBytes(UVChannel.size(), sizeof(FVector2f), PayloadBytes))
				return Fail("Skeletal-mesh payload exceeds the supported byte limit.", &OutError);

		if (Payload.PaletteBoneIndices.empty()
			|| Payload.PaletteBoneIndices.size() != Payload.InverseBindMatrices.size()
			|| Payload.PaletteBoneIndices.size() > SkeletonBoneCount)
			return Fail("Skeletal-mesh palette and inverse-bind counts are invalid.", &OutError);
		std::unordered_set<uint16> Palette;
		for (size_t PaletteIndex = 0; PaletteIndex < Payload.PaletteBoneIndices.size(); ++PaletteIndex)
		{
			const uint16 BoneIndex = Payload.PaletteBoneIndices[PaletteIndex];
			if (BoneIndex >= SkeletonBoneCount || !Palette.insert(BoneIndex).second)
				return Fail("Skeletal-mesh palette contains an invalid or duplicate bone index.", &OutError);
			if (!IsFinite(Payload.InverseBindMatrices[PaletteIndex]))
				return Fail("Skeletal-mesh payload contains a non-finite inverse-bind matrix.", &OutError);
		}

		for (const FSkeletalMeshVertexInfluences& Vertex : Payload.Influences)
		{
			if (Vertex.Count == 0 || Vertex.Count > MaximumSkeletalMeshInfluences)
				return Fail("Skeletal-mesh vertex influence count is invalid.", &OutError);
			float Sum = 0.0f;
			std::unordered_set<uint16> VertexBones;
			for (uint8 InfluenceIndex = 0; InfluenceIndex < Vertex.Count; ++InfluenceIndex)
			{
				const uint16 BoneIndex = Vertex.BoneIndices[InfluenceIndex];
				const float Weight = Vertex.Weights[InfluenceIndex];
				if (!Palette.contains(BoneIndex) || !VertexBones.insert(BoneIndex).second
					|| !std::isfinite(Weight) || Weight <= 0.0f)
					return Fail("Skeletal-mesh vertex contains an invalid bone or weight.", &OutError);
				if (InfluenceIndex > 0)
				{
					const float PreviousWeight = Vertex.Weights[InfluenceIndex - 1];
					const uint16 PreviousBone = Vertex.BoneIndices[InfluenceIndex - 1];
					if (Weight > PreviousWeight || (Weight == PreviousWeight && BoneIndex <= PreviousBone))
						return Fail("Skeletal-mesh influences are not in canonical order.", &OutError);
				}
				Sum += Weight;
			}
			for (uint8 InfluenceIndex = Vertex.Count; InfluenceIndex < MaximumSkeletalMeshInfluences; ++InfluenceIndex)
				if (Vertex.BoneIndices[InfluenceIndex] != 0 || Vertex.Weights[InfluenceIndex] != 0.0f)
					return Fail("Skeletal-mesh unused influence slots must be zero.", &OutError);
			if (std::abs(Sum - 1.0f) > 1.0e-5f)
				return Fail("Skeletal-mesh vertex weights do not sum to one.", &OutError);
		}

		uint64 CoveredIndices = 0;
		for (const FSkeletalMeshSection& Section : Payload.Sections)
		{
			const uint64 End = static_cast<uint64>(Section.FirstIndex) + Section.IndexCount;
			if (Section.Name.IsNone() || Section.IndexCount == 0 || Section.IndexCount % 3 != 0
				|| Section.FirstIndex != CoveredIndices || End > IndexCount
				|| Section.MinVertexIndex > Section.MaxVertexIndex || Section.MaxVertexIndex >= VertexCount
				|| Section.MaterialSlotIndex >= MaterialSlotCount || !IsFinite(Section.LocalBounds))
				return Fail("Skeletal-mesh payload section is invalid or does not form a contiguous partition.", &OutError);
			uint32 ActualMinimum = std::numeric_limits<uint32>::max();
			uint32 ActualMaximum = 0;
			for (uint64 IndexOffset = Section.FirstIndex; IndexOffset < End; ++IndexOffset)
			{
				const uint32 VertexIndex = Payload.Indices[static_cast<size_t>(IndexOffset)];
				ActualMinimum = std::min(ActualMinimum, VertexIndex);
				ActualMaximum = std::max(ActualMaximum, VertexIndex);
				if (!Contains(Section.LocalBounds, Payload.Positions[VertexIndex]))
					return Fail("Skeletal-mesh section bounds do not contain its indexed geometry.", &OutError);
			}
			if (ActualMinimum != Section.MinVertexIndex || ActualMaximum != Section.MaxVertexIndex)
				return Fail("Skeletal-mesh section vertex range does not match its indices.", &OutError);
			CoveredIndices = End;
		}
		if (CoveredIndices != IndexCount)
			return Fail("Skeletal-mesh sections do not cover the complete index buffer.", &OutError);
		OutError.clear();
		return true;
	}

	auto ValidateSkeletalMeshPayload(
		const FSkeletalMeshPayloadData& Payload,
		const DSkeleton& Skeleton,
		uint32 MaterialSlotCount,
		std::string& OutError) -> bool
	{
		return ValidateSkeletalMeshPayload(
			Payload, Skeleton.GetBoneCount(), MaterialSlotCount, OutError);
	}

	auto FSkeletalMeshImportedData::Capture(
		const FSkeletalMeshPayloadData& Payload,
		uint32 SkeletonBoneCount,
		uint32 MaterialSlotCount,
		std::string& OutError) -> bool
	{
		if (!ValidateSkeletalMeshPayload(
			Payload, SkeletonBoneCount, MaterialSlotCount, OutError)) return false;
		FByteArray Bytes;
		FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::BulkData);
		const_cast<FSkeletalMeshPayloadData&>(Payload).Serialize(Ar, {
			.SkeletonBoneCount = SkeletonBoneCount,
			.MaterialSlotCount = MaterialSlotCount,
			.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
			.TargetProfile = ESkeletalPayloadTargetProfile::Game});
		if (Ar.HasError() || Bytes.empty()
			|| Bytes.size() > MaximumSkeletalMeshImportedDataBytes)
			return Fail(Ar.HasError() ? Ar.GetFailure()->Message
				: "SkeletalMesh canonical imported data exceeds its authored bound.",
				&OutError);
		if (!Geometry.UpdatePayload(Bytes))
			return Fail("SkeletalMesh canonical imported data could not be retained.", &OutError);
		SchemaVersion = SkeletalMeshImportedDataSchemaVersion;
		OutError.clear();
		return true;
	}

	auto FSkeletalMeshImportedData::Decode(
		uint32 SkeletonBoneCount,
		uint32 MaterialSlotCount,
		std::string& OutError) const -> FSkeletalMeshPayloadData
	{
		FSkeletalMeshPayloadData Result;
		const FPackageResourceReadResult Payload = Geometry.GetPayload().Wait();
		const std::span<const std::byte> Bytes = Payload.Buffer.GetBytes();
		if (SchemaVersion != SkeletalMeshImportedDataSchemaVersion
			|| !Payload || Bytes.empty()
			|| Bytes.size() > MaximumSkeletalMeshImportedDataBytes)
		{
			OutError = "SkeletalMesh canonical imported-data header is missing or invalid.";
			return Result;
		}
		FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::BulkData);
		Result.Serialize(Ar, {
			.SkeletonBoneCount = SkeletonBoneCount,
			.MaterialSlotCount = MaterialSlotCount,
			.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
			.TargetProfile = ESkeletalPayloadTargetProfile::Game});
		if (Ar.HasError() || !RequireArchiveEnd(Ar)
			|| !ValidateSkeletalMeshPayload(
				Result, SkeletonBoneCount, MaterialSlotCount, OutError))
		{
			if (OutError.empty()) OutError = Ar.HasError()
				? Ar.GetFailure()->Message
				: "SkeletalMesh canonical imported data is invalid.";
			return {};
		}
		OutError.clear();
		return Result;
	}

	auto FSkeletalMeshImportedData::IsValid(
		uint32 SkeletonBoneCount,
		uint32 MaterialSlotCount) const -> bool
	{
		(void)SkeletonBoneCount;
		(void)MaterialSlotCount;
		return SchemaVersion == SkeletalMeshImportedDataSchemaVersion
			&& Geometry.GetPayloadSize() > 0
			&& Geometry.GetPayloadSize() <= MaximumSkeletalMeshImportedDataBytes;
	}

	auto FSkeletalMeshImportedData::GetIdentity() const -> FXxHash128
	{
		if (SchemaVersion != SkeletalMeshImportedDataSchemaVersion
			|| Geometry.GetPayloadSize() == 0) return {};
		FXxHash128Builder Builder;
		Builder.UpdateValue(SchemaVersion);
		Builder.UpdateValue(Geometry.GetPayloadId());
		return Builder.Finalize();
	}

	DSkeletalMesh::DSkeletalMesh(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer) {}

	DSkeletalMesh::~DSkeletalMesh() = default;

	auto DSkeletalMesh::GetPayloadData() const
		-> std::shared_ptr<const FSkeletalMeshPayloadData>
	{
		return PayloadData;
	}

	auto DSkeletalMesh::GetRenderData() const -> const FSkeletalMeshRenderData*
	{
		return RenderData.get();
	}

	auto DSkeletalMesh::RequestRenderDataAndResources() -> FCookedMeshLoadStatus
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (RenderData)
		{
			CookedLoadPhase.store(ECookedMeshCpuPhase::CpuReady, std::memory_order_release);
			if (RenderResourceState.load(std::memory_order_acquire)
				== ERenderResourceState::Uninitialized)
				InitResources();
		}
		else if (CookedLoadPhase.load(std::memory_order_acquire)
			== ECookedMeshCpuPhase::Unloaded)
		{
			SubmitCookedRenderDataRequest(true);
		}
		return GetRenderDataLoadStatus();
	}

	auto DSkeletalMesh::GetRenderDataLoadStatus() const -> FCookedMeshLoadStatus
	{
		const FSkeletalMeshRenderResourceStatus Resource = GetRenderResourceStatus();
		ECookedMeshGpuPhase GpuPhase = ECookedMeshGpuPhase::Unavailable;
		switch (Resource.Readiness)
		{
		case ESkeletalMeshRenderResourceReadiness::Queued: GpuPhase = ECookedMeshGpuPhase::Queued; break;
		case ESkeletalMeshRenderResourceReadiness::Ready: GpuPhase = ECookedMeshGpuPhase::Ready; break;
		case ESkeletalMeshRenderResourceReadiness::Failed: GpuPhase = ECookedMeshGpuPhase::Failed; break;
		case ESkeletalMeshRenderResourceReadiness::Unavailable: break;
		}
		return {.CpuPhase = RenderData ? ECookedMeshCpuPhase::CpuReady
			: CookedLoadPhase.load(std::memory_order_acquire),
			.GpuPhase = GpuPhase,
			.Generation = CookedLoadGeneration.load(std::memory_order_acquire),
			.ResourceRevision = Resource.Revision};
	}

	auto DSkeletalMesh::EnsureRenderDataLoadedBlocking()
		-> FCookedMeshBlockingResult
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		const ECookedMeshCpuPhase Phase =
			CookedLoadPhase.load(std::memory_order_acquire);
		if (Phase == ECookedMeshCpuPhase::Failed
			|| Phase == ECookedMeshCpuPhase::Cancelled)
		{
			if (FCookedMeshLoadManager* Manager = GetCookedMeshLoadManager())
				Manager->Cancel(MakeObjectHandle(this));
			CookedLoadPhase.store(ECookedMeshCpuPhase::Unloaded, std::memory_order_release);
			CookedLoadGeneration.fetch_add(1, std::memory_order_acq_rel);
		}
		if (!RenderData && CookedLoadPhase.load(std::memory_order_acquire)
			== ECookedMeshCpuPhase::Unloaded)
			SubmitCookedRenderDataRequest(false);
		const FCookedMeshLoadStatus Initial = GetRenderDataLoadStatus();
		if (!Initial.HasCpuData() && Initial.CpuPhase != ECookedMeshCpuPhase::Failed)
		{
			if (FCookedMeshLoadManager* Manager = GetCookedMeshLoadManager();
				Manager && Initial.CpuPhase != ECookedMeshCpuPhase::Unloaded)
			{
				Manager->Finish(MakeObjectHandle(this));
			}
		}
		if (!RenderData && CookedLoadPhase.load(std::memory_order_acquire)
			!= ECookedMeshCpuPhase::Failed
			&& GetAssetRuntimeConfiguration().RequiresCookedPayload()
			&& CookedPlatformData.GetMetadata().LogicalSize != 0)
		{
			CookedLoadPhase.store(ECookedMeshCpuPhase::Reading, std::memory_order_release);
			std::string Error;
			if ((!PayloadData && !LoadCookedPayload(Error))
				|| (!RenderData && !BuildRenderData(Error)))
			{
				CookedLoadPhase.store(ECookedMeshCpuPhase::Failed, std::memory_order_release);
				return {.Status = GetRenderDataLoadStatus(), .Message = std::move(Error)};
			}
			CookedLoadPhase.store(ECookedMeshCpuPhase::CpuReady, std::memory_order_release);
		}
		if (RenderData)
		{
			CookedLoadPhase.store(ECookedMeshCpuPhase::CpuReady, std::memory_order_release);
		}
		FCookedMeshBlockingResult Result{.Status = GetRenderDataLoadStatus()};
		if (!Result.Status.HasCpuData()) Result.Message = "SkeletalMesh CPU render data is unavailable.";
		return Result;
	}

	auto DSkeletalMesh::GetMaterialSlot(uint32 SlotIndex) const
		-> const FMeshMaterialSlotDefinition*
	{
		return SlotIndex < MaterialSlots.size() ? &MaterialSlots[SlotIndex] : nullptr;
	}

	auto DSkeletalMesh::FindMaterialSlot(FName Name) const
		-> const FMeshMaterialSlotDefinition*
	{
		const auto It = std::ranges::find(MaterialSlots, Name,
			&FMeshMaterialSlotDefinition::Name);
		return It != MaterialSlots.end() ? &*It : nullptr;
	}

	auto DSkeletalMesh::BuildRenderData(std::string& OutError) -> bool
	{
		if (!Skeleton || !PayloadData)
			return Fail("SkeletalMesh render data requires a Skeleton and payload.", &OutError);
		std::unique_ptr<FSkeletalMeshRenderData> Candidate;
		if (!BuildSkeletalMeshRenderData(*PayloadData, *Skeleton, MeshNodeBindTransform,
			MaterialSlots, Candidate, OutError)) return false;
		if (RenderData && RenderData->GetNumInitializedResources() != 0)
			return Fail("Initialized SkeletalMesh render data must be replaced through imported-state exchange.", &OutError);
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
		ERenderResourceState Expected = RenderResourceState.load(std::memory_order_acquire);
		if ((Expected != ERenderResourceState::Uninitialized
				&& Expected != ERenderResourceState::Failed)
			|| !RenderResourceState.compare_exchange_strong(
				Expected, ERenderResourceState::InitializationQueued, std::memory_order_acq_rel))
			return;
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

	auto DSkeletalMesh::SetAssetData(
		FSkeletalMeshAssetData InData,
		std::string& OutError) -> bool
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!InData.Skeleton || !InData.Payload)
			return Fail("Skeletal-mesh imported data requires a Skeleton and payload.", &OutError);
		const DSkeleton* ValidationSkeleton = InData.ValidationSkeleton
			? InData.ValidationSkeleton : InData.Skeleton;
		if (InData.SkeletonCompatibilityIdentity != ValidationSkeleton->GetCompatibilityIdentity())
			return Fail("Skeletal-mesh imported data is incompatible with its Skeleton.", &OutError);
		if (!InData.MeshNodeBindTransform.IsValid(&OutError))
		{
			OutError = std::format("Skeletal-mesh bind transform is invalid: {}", OutError);
			return false;
		}
		if (InData.MaterialSlots.empty() || InData.MaterialSlots.size() > MaximumMeshMaterialSlots)
			return Fail("Skeletal-mesh material-slot count is outside the supported range.", &OutError);
		std::unordered_set<FName> Names;
		std::unordered_set<uint32> SourceIndices;
		for (const FMeshMaterialSlotDefinition& Slot : InData.MaterialSlots)
			if (Slot.Name.IsNone() || !Names.insert(Slot.Name).second
				|| !SourceIndices.insert(Slot.SourceMaterialIndex).second)
				return Fail("Skeletal-mesh material slots require unique non-None names and source indices.", &OutError);
		if (!ValidateSkeletalMeshPayload(
			*InData.Payload, *ValidationSkeleton,
			static_cast<uint32>(InData.MaterialSlots.size()), OutError)) return false;
		FSkeletalMeshImportedData ImportedCandidate = InData.ImportedData.value_or(FSkeletalMeshImportedData{});
		if (!InData.ImportedData
			&& !ImportedCandidate.Capture(*InData.Payload,
				ValidationSkeleton->GetBoneCount(),
				static_cast<uint32>(InData.MaterialSlots.size()), OutError)) return false;
		if (!ImportedCandidate.IsValid(ValidationSkeleton->GetBoneCount(),
				static_cast<uint32>(InData.MaterialSlots.size())))
			return Fail("SkeletalMesh canonical imported data is missing or invalid.", &OutError);

		std::unique_ptr<FSkeletalMeshRenderData> RenderDataCandidate;
		if (!BuildSkeletalMeshRenderData(*InData.Payload, *ValidationSkeleton,
			InData.MeshNodeBindTransform, InData.MaterialSlots,
			RenderDataCandidate, OutError)) return false;
		if (RenderData && RenderData->GetNumInitializedResources() != 0)
			return Fail("Initialized SkeletalMesh state must be replaced through imported-state exchange.", &OutError);

		Skeleton = InData.Skeleton;
		SkeletonCompatibilityIdentity = std::move(InData.SkeletonCompatibilityIdentity);
		MeshNodeBindTransform = InData.MeshNodeBindTransform;
		MaterialSlots = std::move(InData.MaterialSlots);
		Summary = {
			.VertexCount = static_cast<uint32>(InData.Payload->Positions.size()),
			.IndexCount = static_cast<uint32>(InData.Payload->Indices.size()),
			.SectionCount = static_cast<uint32>(InData.Payload->Sections.size()),
			.LocalBounds = FSkeletalMeshBounds::FromBox(InData.Payload->LocalBounds)};
		CookedPlatformData = {};
		ImportedData = std::move(ImportedCandidate);
		PayloadData = std::move(InData.Payload);
		RenderData = std::move(RenderDataCandidate);
		RenderResourceState.store(ERenderResourceState::Uninitialized, std::memory_order_release);
		OutError.clear();
		return true;
	}

	auto DSkeletalMesh::Validate(std::string& OutError) const -> bool
	{
		if (!Skeleton)
			return Fail("SkeletalMesh has no Skeleton reference.", &OutError);
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
			return Fail("SkeletalMesh compatibility identity does not match its Skeleton.", &OutError);
		if (!MeshNodeBindTransform.IsValid(&OutError))
		{
			OutError = std::format("SkeletalMesh bind transform is invalid: {}", OutError);
			return false;
		}
		if (MaterialSlots.empty() || MaterialSlots.size() > MaximumMeshMaterialSlots)
			return Fail("SkeletalMesh material-slot count is outside the supported range.", &OutError);
		std::unordered_set<FName> Names;
		std::unordered_set<uint32> SourceIndices;
		const bool bRequireSourceIndices =
			GetAssetRuntimeConfiguration().AllowsSourceFallback();
		for (const FMeshMaterialSlotDefinition& Slot : MaterialSlots)
			if (Slot.Name.IsNone() || !Names.insert(Slot.Name).second
				|| (bRequireSourceIndices
					&& !SourceIndices.insert(Slot.SourceMaterialIndex).second))
				return Fail("SkeletalMesh material slots are not canonical and unique.", &OutError);
		if (Summary.VertexCount == 0 || Summary.VertexCount > MaximumSkeletalMeshVertices
			|| Summary.IndexCount == 0 || Summary.IndexCount > MaximumSkeletalMeshIndices
			|| Summary.IndexCount % 3 != 0 || Summary.SectionCount == 0
			|| Summary.SectionCount > MaximumSkeletalMeshSections || !Summary.LocalBounds.IsValid(&OutError))
			return Fail("SkeletalMesh authored summary is invalid.", &OutError);
		if (PayloadData)
		{
			if (!ValidateSkeletalMeshPayload(
				*PayloadData, ProspectiveSkeleton, static_cast<uint32>(MaterialSlots.size()), OutError)) return false;
			if (PayloadData->Positions.size() != Summary.VertexCount
				|| PayloadData->Indices.size() != Summary.IndexCount
				|| PayloadData->Sections.size() != Summary.SectionCount
				|| FSkeletalMeshBounds::FromBox(PayloadData->LocalBounds) != Summary.LocalBounds)
				return Fail("SkeletalMesh payload does not match its authored summary.", &OutError);
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
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (CookedPlatformData.GetMetadata().LogicalSize == 0)
				return Fail(std::format(
					"Cooked SkeletalMesh '{}': required PlatformData field is missing.",
					GetObjectPath()), &OutError);
			PayloadData.reset();
			RenderData.reset();
			OutError.clear();
			return true;
		}
		if (!PayloadData && !PrepareSkeletalMeshPayload(*this, OutError)) return false;
		return PayloadData && (RenderData || BuildRenderData(OutError));
	}

	auto DSkeletalMesh::SerializeCooked(FArchive& Ar) -> void
	{
		Super::SerializeCooked(Ar);
		if (Ar.GetTarget().Platform != "Win64" || Ar.GetTarget().Profile != "Game")
		{
			Ar.Fail(EArchiveFailureCode::InvalidData,
				"SkeletalMesh cooked platform data requires the Win64 Game target.");
			return;
		}
		FBulkData Projection;
		FBulkData* FieldValue = &CookedPlatformData;
		if (Ar.IsSaving())
		{
			if (!PayloadData || !Skeleton)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"SkeletalMesh cooked platform data is unavailable.");
				return;
			}
			FByteArray Bytes;
			FCanonicalMemoryWriter Writer(Bytes, EArchivePurpose::CookedPayload);
			const_cast<FSkeletalMeshPayloadData&>(*PayloadData).Serialize(Writer, {
				.SkeletonBoneCount = Skeleton->GetBoneCount(),
				.MaterialSlotCount = static_cast<uint32>(MaterialSlots.size()),
				.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
				.TargetProfile = ESkeletalPayloadTargetProfile::Game});
			std::string Error;
			if (Writer.HasError()
				|| !FBulkData::TryCreateDetached(Bytes, Projection, &Error))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					Error.empty() ? std::string(Writer.GetError()) : std::move(Error));
				return;
			}
			FieldValue = &Projection;
		}
		auto Field = EnterArchiveField(Ar, {FName("Durin::DSkeletalMesh"),
			FName("PlatformData"), FArchiveLogicalTypeDescriptor::BulkData()});
		FieldValue->Serialize(Ar, {.Alignment = EditorBulkDataExternalAlignment,
			.StoragePolicy = EArchiveBulkDataStoragePolicy::AllowExternal});
	}

	auto DSkeletalMesh::LoadCookedPayload(std::string& OutError) -> bool
	{
		auto FailCooked = [&](std::string Message) {
			CookedLoadPhase.store(ECookedMeshCpuPhase::Failed, std::memory_order_release);
			OutError = std::format("Cooked SkeletalMesh '{}': {}", GetObjectPath(), Message);
			return false;
		};
		std::span<const std::byte> Bytes;
		if (!CookedPlatformData.LockReadOnly(Bytes, &OutError))
			return FailCooked(OutError);
		if (!Skeleton)
		{
			CookedPlatformData.UnlockReadOnly();
			return FailCooked("Skeleton is unavailable.");
		}
		FSkeletalMeshCookedProduct Product;
		FCookedMeshProductError ProductError;
		if (!DecodeSkeletalMeshCookedProduct(Bytes, Skeleton->GetBones(),
			MeshNodeBindTransform, MaterialSlots, Summary, Product, ProductError))
		{
			CookedPlatformData.UnlockReadOnly();
			return FailCooked(std::move(ProductError.Message));
		}
		if (!CookedPlatformData.UnlockReadOnly(&OutError)) return FailCooked(OutError);
		PayloadData = std::move(Product.Payload);
		RenderData = std::move(Product.RenderData);
		RenderResourceState.store(ERenderResourceState::Uninitialized, std::memory_order_release);
		RenderResourceRevision.fetch_add(1, std::memory_order_acq_rel);
		OutError.clear();
		return true;
	}

	auto DSkeletalMesh::SubmitCookedRenderDataRequest(bool bInitializeResources) -> bool
	{
		FCookedMeshLoadManager* Manager = GetCookedMeshLoadManager();
		if (!Manager || RenderData
			|| !GetAssetRuntimeConfiguration().RequiresCookedPayload()
			|| CookedPlatformData.GetMetadata().LogicalSize == 0 || !Skeleton)
			return false;

		const uint64 Generation = CookedLoadGeneration.load(std::memory_order_acquire);
		const uint64 ResourceRevision = GetRenderResourceStatus().Revision;
		const uint64 MetadataIdentity = BuildSkeletalCookedMetadataIdentity(*this);
		std::vector<FSkeletonBone> BoneSnapshot(
			Skeleton->GetBones().begin(), Skeleton->GetBones().end());
		FSkeletonTransform TransformSnapshot = MeshNodeBindTransform;
		std::vector<FMeshMaterialSlotDefinition> SlotSnapshot = MaterialSlots;
		FSkeletalMeshSummary SummarySnapshot = Summary;

		FCookedMeshLoadRequest Request{
			.Identity = {
				.Owner = MakeObjectHandle(this),
				.Family = ECookedMeshFamily::SkeletalMesh,
				.LoadGeneration = Generation,
				.ResourceRevision = ResourceRevision,
				.MetadataIdentity = MetadataIdentity},
			.Fields = {CookedPlatformData},
			.Worker = [Bones = std::move(BoneSnapshot), TransformSnapshot,
				Slots = std::move(SlotSnapshot), SummarySnapshot](
				std::span<const FSharedByteBuffer> Buffers,
				const FTaskCancellationToken& Cancellation)
				-> FCookedMeshWorkerResult {
				if (Cancellation.IsCancellationRequested()) return {};
				if (Buffers.size() != 1)
					return {.Message = "SkeletalMesh cooked field count is invalid."};
				auto Result = std::make_unique<FSkeletalMeshManagerProduct>();
				FCookedMeshProductError Error;
				if (!DecodeSkeletalMeshCookedProduct(Buffers[0].GetBytes(), Bones,
					TransformSnapshot, Slots, SummarySnapshot, Result->Product, Error))
					return {.Message = std::move(Error.Message)};
				return {.Product = std::move(Result),
					.RetainedBytes = std::max<uint64>(Buffers[0].GetSize(), 1)};
			},
			.IsCurrent = [](const DObject& Owner,
				const FCookedMeshLoadIdentity& Identity) {
				const auto* Mesh = Cast<DSkeletalMesh>(&Owner);
				return Mesh
					&& Mesh->CookedLoadGeneration.load(std::memory_order_acquire)
						== Identity.LoadGeneration
					&& Mesh->GetRenderResourceStatus().Revision
						== Identity.ResourceRevision
					&& BuildSkeletalCookedMetadataIdentity(*Mesh)
						== Identity.MetadataIdentity;
			},
			.Publish = [bInitializeResources](DObject& Owner,
				const FCookedMeshLoadIdentity&,
				std::unique_ptr<ICookedMeshDetachedProduct> BaseProduct,
				std::string& OutError) {
				auto* Mesh = Cast<DSkeletalMesh>(&Owner);
				auto* Typed = dynamic_cast<FSkeletalMeshManagerProduct*>(BaseProduct.get());
				if (!Mesh || !Typed)
				{
					OutError = "SkeletalMesh cooked publication product is invalid.";
					return false;
				}
				FSkeletalMeshRenderStateRecreateContext RecreateContext(Mesh);
				FSkeletalMeshCookedProduct Product = std::move(Typed->Product);
				Mesh->PayloadData = std::move(Product.Payload);
				Mesh->RenderData = std::move(Product.RenderData);
				Mesh->RenderResourceState.store(
					ERenderResourceState::Uninitialized, std::memory_order_release);
				Mesh->RenderResourceRevision.fetch_add(1, std::memory_order_acq_rel);
				Mesh->CookedLoadPhase.store(
					ECookedMeshCpuPhase::CpuReady, std::memory_order_release);
				if (bInitializeResources) Mesh->InitResources();
				OutError.clear();
				return true;
			},
			.OnTerminal = [](DObject& Owner,
				const FCookedMeshLoadIdentity&,
				ECookedMeshTerminalState Terminal,
				std::string_view Message) {
				auto* Mesh = Cast<DSkeletalMesh>(&Owner);
				if (!Mesh) return;
				const bool bFailed = Terminal == ECookedMeshTerminalState::Failed
					|| Terminal == ECookedMeshTerminalState::Rejected;
				Mesh->CookedLoadPhase.store(bFailed
					? ECookedMeshCpuPhase::Failed : ECookedMeshCpuPhase::Cancelled,
					std::memory_order_release);
			}
		};
		if (!Manager->Submit(std::move(Request))) return false;
		CookedLoadPhase.store(ECookedMeshCpuPhase::IoQueued, std::memory_order_release);
		return true;
	}

	auto DSkeletalMesh::ContributeToCook(
		FCookContext& Context,
		std::string_view VirtualPackagePath,
		std::string& OutError) -> bool
	{
		if (Context.GetTargetPlatform() != ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != ECookTargetProfile::Game)
			return Fail(std::format(
				"SkeletalMesh '{}' supports only the Win64 game cook target.", GetObjectPath()), &OutError);
		if (!PayloadData && !PostLoad(OutError)) return false;
		if (!PayloadData) return Fail("SkeletalMesh has no CPU payload to cook.", &OutError);
		return Context.AddPackage(std::string(VirtualPackagePath), GetPackage(), &OutError);
	}

	auto DSkeletalMesh::BeginDestroy() -> void
	{
		if (FCookedMeshLoadManager* Manager = GetCookedMeshLoadManager())
			Manager->Cancel(MakeObjectHandle(this));
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
