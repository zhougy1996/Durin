#include "Asset/CookedMeshProducts.h"

#include "Serialization/Archive.h"

namespace Durin
{
	namespace
	{
		auto ArchiveFailure(ECookedMeshProductError Code, const FArchive& Ar, uint64 ByteCount) -> FCookedMeshProductResult
		{
			FCookedMeshProductError Error{.Code = Code, .ByteOffset = Ar.Tell(), .ByteCount = ByteCount};
			if (const auto* Failure = Ar.GetFailure())
			{
				Error.ArchiveCode = Failure->Code;
				Error.ArchivePath = Failure->Path;
			}
			return {std::move(Error)};
		}

		auto RestoreStaticMeshRuntimeMetadata(
			std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
			FStaticMeshRenderData& RenderData) -> FCookedMeshProductResult
		{
			if (RenderData.MaterialSlots.size() != MaterialSlots.size())
				return {{.Code = ECookedMeshProductError::RuntimeMaterialSlotCount,
					.Actual = RenderData.MaterialSlots.size(), .Expected = MaterialSlots.size()}};
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
			return {};
		}
	}

	auto FormatCookedMeshProductError(const FCookedMeshProductError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case ECookedMeshProductError::None: return {};
		case ECookedMeshProductError::MissingCollision: return "Cooked static-mesh collision payload is missing.";
		case ECookedMeshProductError::CollisionArchive:
		case ECookedMeshProductError::RenderArchive:
			return std::format("Cooked static-mesh {} payload decoding failed at byte {} of {} (Archive code {}, path '{}').",
				Error.Code == ECookedMeshProductError::CollisionArchive ? "collision" : "render",
				Error.ByteOffset, Error.ByteCount, Error.ArchiveCode ? static_cast<int>(*Error.ArchiveCode) : -1, Error.ArchivePath);
		case ECookedMeshProductError::CollisionMetadata:
			return std::format("DCOL mode/policy ({}/{}) does not match cooked BodySetup metadata ({}/{}).",
				static_cast<int>(Error.ActualMode), static_cast<int>(Error.ActualPolicy),
				static_cast<int>(Error.ExpectedMode), static_cast<int>(Error.ExpectedPolicy));
		case ECookedMeshProductError::CollisionConstruction:
			return Error.CollisionCause ? FormatStaticMeshCollisionPayloadError(*Error.CollisionCause) : "Cooked collision construction failed.";
		case ECookedMeshProductError::RenderConstruction:
			return Error.RenderCause ? FormatStaticMeshPayloadError(*Error.RenderCause) : "Cooked render construction failed.";
		case ECookedMeshProductError::MaterialSlotCount:
		case ECookedMeshProductError::RuntimeMaterialSlotCount:
			return std::format("Static-mesh material slot count {} does not match package metadata count {}.", Error.Actual, Error.Expected);
		}
		return {};
	}

	auto DecodeStaticMeshCookedProduct(
		FByteView RenderBytes,
		FByteView CollisionBytes,
		std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
		EBodySetupCollisionSourceMode CollisionMode,
		EBodySetupCollisionQueryPolicy CollisionPolicy,
		FStaticMeshCookedProduct& OutProduct) -> FCookedMeshProductResult
	{
		FStaticMeshCookedProduct Candidate;
		if (CollisionMode != EBodySetupCollisionSourceMode::None)
		{
			if (CollisionBytes.empty())
				return {{.Code = ECookedMeshProductError::MissingCollision,
					.ExpectedMode = CollisionMode, .ExpectedPolicy = CollisionPolicy}};
			FStaticMeshCollisionPayloadData CollisionPayload;
			FCanonicalMemoryReader CollisionAr(
				CollisionBytes, EArchivePurpose::CookedPayload, {.Target = {"Win64", "Game"}});
			CollisionPayload.Serialize(CollisionAr);
			if (CollisionAr.HasError() || !RequireArchiveEnd(CollisionAr))
				return ArchiveFailure(ECookedMeshProductError::CollisionArchive, CollisionAr, CollisionBytes.size());
			if (CollisionPayload.SourceMode != CollisionMode
				|| CollisionPayload.QueryPolicy != CollisionPolicy)
			{
				return {{.Code = ECookedMeshProductError::CollisionMetadata,
					.ActualMode = CollisionPayload.SourceMode, .ExpectedMode = CollisionMode,
					.ActualPolicy = CollisionPayload.QueryPolicy, .ExpectedPolicy = CollisionPolicy}};
			}
			FCollisionGeometryRef Geometry;
			if (const auto Built = MakeStaticMeshCollisionGeometry(CollisionPayload, Geometry); !Built)
			{
				return {{.Code = ECookedMeshProductError::CollisionConstruction, .CollisionCause = Built.Error}};
			}
			if (CollisionMode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0)
				Candidate.SimpleCollision = std::move(Geometry);
			else
				Candidate.ComplexCollision = std::move(Geometry);
			Candidate.CollisionPayloadBytes = CollisionBytes.size();
			Candidate.bHasCollision = true;
		}

		FStaticMeshPayloadData Payload;
		FCanonicalMemoryReader PayloadAr(RenderBytes, EArchivePurpose::CookedPayload, {.Target = {"Win64", "Game"}});
		Payload.Serialize(PayloadAr);
		if (PayloadAr.HasError() || !RequireArchiveEnd(PayloadAr))
			return ArchiveFailure(ECookedMeshProductError::RenderArchive, PayloadAr, RenderBytes.size());
		if (Payload.MaterialSlotCount != MaterialSlots.size())
			return {{.Code = ECookedMeshProductError::MaterialSlotCount,
				.Actual = Payload.MaterialSlotCount, .Expected = MaterialSlots.size()}};
		if (const auto Result = MakeStaticMeshRenderData(Payload, Candidate.RenderData); !Result)
		{
			return {{.Code = ECookedMeshProductError::RenderConstruction, .RenderCause = Result.Error}};
		}
		if (const auto Result = RestoreStaticMeshRuntimeMetadata(MaterialSlots, *Candidate.RenderData); !Result)
			return Result;
		OutProduct = std::move(Candidate);
		return {};
	}
}
