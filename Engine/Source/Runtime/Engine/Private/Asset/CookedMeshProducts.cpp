#include "Asset/CookedMeshProducts.h"

#include "Serialization/Archive.h"
#include "SkeletalMesh/SkeletalDerivedData.h"
#include "SkeletalMesh/SkeletalMeshResources.h"

namespace Durin
{
	namespace
	{
		auto CookedMeshProductFail(
			FCookedMeshProductError& OutError,
			ECookedMeshProductFailure Category,
			std::string Message) -> bool
		{
			OutError = {.Category = Category, .Message = std::move(Message)};
			return false;
		}

		auto RestoreStaticMeshRuntimeMetadata(
			std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
			FStaticMeshRenderData& RenderData,
			FCookedMeshProductError& OutError) -> bool
		{
			if (RenderData.MaterialSlots.size() != MaterialSlots.size())
				return CookedMeshProductFail(OutError, ECookedMeshProductFailure::Metadata,
					"Cached static-mesh material slot count does not match asset metadata.");
			for (size_t SlotIndex = 0; SlotIndex < MaterialSlots.size(); ++SlotIndex)
			{
				const FMeshMaterialSlotDefinition& Definition = MaterialSlots[SlotIndex];
				FStaticMeshMaterialSlot& Slot = RenderData.MaterialSlots[SlotIndex];
				Slot.Name = Definition.Name.ToString();
				Slot.SourceMaterialIndex = Definition.SourceMaterialIndex;
			}
			for (size_t LODIndex = 0; LODIndex < RenderData.LODResources.size(); ++LODIndex)
				for (size_t SectionIndex = 0;
					SectionIndex < RenderData.LODResources[LODIndex].Sections.size(); ++SectionIndex)
				{
					RenderData.LODResources[LODIndex].Sections[SectionIndex].Name =
						std::format("LOD{}_Section{}", LODIndex, SectionIndex);
				}
			return true;
		}
	}

	auto DecodeStaticMeshCookedProduct(
		std::span<const std::byte> RenderBytes,
		std::span<const std::byte> CollisionBytes,
		std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
		EBodySetupCollisionSourceMode CollisionMode,
		EBodySetupCollisionQueryPolicy CollisionPolicy,
		FStaticMeshCookedProduct& OutProduct,
		FCookedMeshProductError& OutError) -> bool
	{
		FStaticMeshCookedProduct Candidate;
		if (CollisionMode != EBodySetupCollisionSourceMode::None)
		{
			if (CollisionBytes.empty())
				return CookedMeshProductFail(OutError, ECookedMeshProductFailure::Schema,
					"Cooked static-mesh collision payload is missing.");
			FStaticMeshCollisionPayloadData CollisionPayload;
			FCanonicalMemoryReader CollisionAr(
				CollisionBytes, EArchivePurpose::CookedPayload);
			CollisionPayload.Serialize(CollisionAr, EStaticMeshTargetPlatform::Win64);
			if (CollisionAr.HasError() || !RequireArchiveEnd(CollisionAr))
				return CookedMeshProductFail(OutError, ECookedMeshProductFailure::Schema,
					std::string(CollisionAr.GetError()));
			if (CollisionPayload.SourceMode != CollisionMode
				|| CollisionPayload.QueryPolicy != CollisionPolicy)
			{
				return CookedMeshProductFail(OutError, ECookedMeshProductFailure::Metadata,
					"DCOL policy does not match its cooked BodySetup metadata.");
			}
			FCollisionGeometryRef Geometry;
			std::string Error;
			if (!MakeStaticMeshCollisionGeometry(CollisionPayload, Geometry, Error))
				return CookedMeshProductFail(OutError, ECookedMeshProductFailure::Construction,
					std::move(Error));
			if (CollisionMode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0)
				Candidate.SimpleCollision = std::move(Geometry);
			else
				Candidate.ComplexCollision = std::move(Geometry);
			Candidate.CollisionPayloadBytes = CollisionBytes.size();
			Candidate.bHasCollision = true;
		}

		FStaticMeshPayloadData Payload;
		FCanonicalMemoryReader PayloadAr(RenderBytes, EArchivePurpose::CookedPayload);
		Payload.Serialize(PayloadAr, EStaticMeshTargetPlatform::Win64);
		if (PayloadAr.HasError() || !RequireArchiveEnd(PayloadAr))
			return CookedMeshProductFail(OutError, ECookedMeshProductFailure::Schema,
				std::string(PayloadAr.GetError()));
		if (Payload.MaterialSlotCount != MaterialSlots.size())
			return CookedMeshProductFail(OutError, ECookedMeshProductFailure::Metadata,
				"Static-mesh payload material slot count does not match package metadata.");
		std::string Error;
		if (!MakeStaticMeshRenderData(Payload, Candidate.RenderData, Error))
			return CookedMeshProductFail(OutError, ECookedMeshProductFailure::Construction,
				std::move(Error));
		if (!RestoreStaticMeshRuntimeMetadata(
			MaterialSlots, *Candidate.RenderData, OutError)) return false;
		OutProduct = std::move(Candidate);
		OutError = {};
		return true;
	}

	auto DecodeSkeletalMeshCookedProduct(
		std::span<const std::byte> Bytes,
		std::span<const FSkeletonBone> SkeletonBones,
		const FSkeletonTransform& MeshNodeBindTransform,
		std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
		const FSkeletalMeshSummary& ExpectedSummary,
		FSkeletalMeshCookedProduct& OutProduct,
		FCookedMeshProductError& OutError) -> bool
	{
		FSkeletalMeshPayloadData Payload;
		FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::CookedPayload);
		Payload.Serialize(Ar, {
			.SkeletonBoneCount = static_cast<uint32>(SkeletonBones.size()),
			.MaterialSlotCount = static_cast<uint32>(MaterialSlots.size()),
			.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
			.TargetProfile = ESkeletalPayloadTargetProfile::Game});
		if (Ar.HasError() || !RequireArchiveEnd(Ar))
			return CookedMeshProductFail(OutError, ECookedMeshProductFailure::Schema,
				std::string(Ar.GetError()));
		if (Payload.Positions.size() != ExpectedSummary.VertexCount
			|| Payload.Indices.size() != ExpectedSummary.IndexCount
			|| Payload.Sections.size() != ExpectedSummary.SectionCount
			|| FSkeletalMeshBounds::FromBox(Payload.LocalBounds) != ExpectedSummary.LocalBounds)
		{
			return CookedMeshProductFail(OutError, ECookedMeshProductFailure::Metadata,
				"Skeletal-mesh payload does not match authored summary.");
		}
		FSkeletalMeshCookedProduct Candidate;
		std::string Error;
		if (!BuildSkeletalMeshRenderData(Payload, SkeletonBones,
			MeshNodeBindTransform, MaterialSlots, Candidate.RenderData, Error))
		{
			return CookedMeshProductFail(OutError, ECookedMeshProductFailure::Construction,
				std::move(Error));
		}
		Candidate.Payload = std::make_shared<const FSkeletalMeshPayloadData>(
			std::move(Payload));
		OutProduct = std::move(Candidate);
		OutError = {};
		return true;
	}
}
