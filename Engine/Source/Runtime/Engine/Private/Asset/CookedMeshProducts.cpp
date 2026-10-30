#include "Physics/PhysicsDerivedData.h"
#include "Asset/CookedMeshProducts.h"
#include "CookedMeshProductsPrivate.h"
#include "Logging/LogMacros.h"

#include "Serialization/Archive.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace Durin
{
	using namespace AssetPrivate;
	namespace
	{
		auto ArchiveFailure(ECookedMeshProductError Code, const FArchive& Ar, uint64 ByteCount) -> std::expected<void, FCookedMeshProductError>
		{
			FCookedMeshProductError Error{.Code = Code, .ByteOffset = Ar.Tell(), .ByteCount = ByteCount};
			if (const auto* Failure = Ar.GetFailure())
			{
				Error.ArchiveCode = Failure->Code;
				Error.ArchivePath = Failure->Path;
			}
			return std::unexpected(std::move(Error));
		}

		auto RestoreStaticMeshRuntimeMetadata(
			std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
			FStaticMeshRenderData& RenderData
		) -> std::expected<void, FCookedMeshProductError>
		{
			if (RenderData.MaterialSlots.size() != MaterialSlots.size())
				return std::unexpected(FCookedMeshProductError{.Code = ECookedMeshProductError::RuntimeMaterialSlotCount, .Actual = RenderData.MaterialSlots.size(), .Expected = MaterialSlots.size()});
			for (size_t SlotIndex = 0; SlotIndex < MaterialSlots.size(); ++SlotIndex)
			{
				const FMeshMaterialSlotDefinition& Definition = MaterialSlots[SlotIndex];
				FStaticMeshMaterialSlot& Slot = RenderData.MaterialSlots[SlotIndex];
				Slot.Name = Definition.Name.ToString();
				// Cooked packages strip source reconciliation metadata.
#if DURIN_WITH_EDITORONLY_DATA
				Slot.SourceMaterialIndex = Definition.SourceMaterialIndex;
#else
				Slot.SourceMaterialIndex = 0;
#endif
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

	auto AssetPrivate::FormatCookedMeshProductError(const FCookedMeshProductError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case ECookedMeshProductError::MissingCollision: return "Cooked static-mesh collision payload is missing.";
		case ECookedMeshProductError::CollisionArchive:
		case ECookedMeshProductError::RenderArchive:
			return std::format("Cooked static-mesh {} payload decoding failed at byte {} of {} (Archive code {}, path '{}').",
				Error.Code == ECookedMeshProductError::CollisionArchive ? "collision" : "render",
				Error.ByteOffset, Error.ByteCount, Error.ArchiveCode ? static_cast<int>(*Error.ArchiveCode) : -1, Error.ArchivePath);
		case ECookedMeshProductError::CollisionMetadata:
			return std::format("DCOL mode ({}) does not match cooked BodySetup metadata ({}).",
				static_cast<int>(Error.ActualMode), static_cast<int>(Error.ExpectedMode));
		case ECookedMeshProductError::CollisionConstruction:
			return Error.CollisionCause ? PhysicsPrivate::FormatPhysicsCollisionPayloadError(*Error.CollisionCause) : "Cooked collision construction failed.";
		case ECookedMeshProductError::RenderConstruction:
			return Error.RenderCause ? StaticMeshPrivate::FormatStaticMeshPayloadError(*Error.RenderCause) : "Cooked render construction failed.";
		case ECookedMeshProductError::MaterialSlotCount:
		case ECookedMeshProductError::RuntimeMaterialSlotCount:
			return std::format("Static-mesh material slot count {} does not match package metadata count {}.", Error.Actual, Error.Expected);
		}
		return {};
	}

	auto AssetPrivate::TryDecodeStaticMeshCookedProduct(
		FByteView RenderBytes,
		FByteView CollisionBytes,
		std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
		EBodySetupCollisionSourceMode CollisionMode,
		FStaticMeshCookedProduct& OutProduct,
		ECookTargetPlatform TargetPlatform
	) -> std::expected<void, FCookedMeshProductError>
	{
		const FArchiveTarget ArchiveTarget{.Platform = TargetPlatform == ECookTargetPlatform::MacOS
			? "MacOS" : "Win64", .Profile = "Game"};
		FStaticMeshCookedProduct Candidate;
		if (CollisionMode != EBodySetupCollisionSourceMode::None)
		{
			if (CollisionBytes.empty())
				return std::unexpected(FCookedMeshProductError{.Code = ECookedMeshProductError::MissingCollision, .ExpectedMode = CollisionMode});
			FPhysicsCollisionPayloadData CollisionPayload;
			FCanonicalMemoryReader CollisionAr(
				CollisionBytes, EArchivePurpose::CookedPayload, {.Target = ArchiveTarget});
			CollisionPayload.Serialize(CollisionAr);
			if (CollisionAr.IsError() || !RequireArchiveEnd(CollisionAr))
				return ArchiveFailure(ECookedMeshProductError::CollisionArchive, CollisionAr, CollisionBytes.size());
			if (CollisionPayload.SourceMode != CollisionMode)
			{
				return std::unexpected(FCookedMeshProductError{.Code = ECookedMeshProductError::CollisionMetadata, .ActualMode = CollisionPayload.SourceMode, .ExpectedMode = CollisionMode});
			}
			FCollisionGeometryRef Geometry;
			if (const auto Built = PhysicsPrivate::TryMakePhysicsCollisionGeometry(CollisionPayload, Geometry); !Built)
			{
				return std::unexpected(FCookedMeshProductError{.Code = ECookedMeshProductError::CollisionConstruction, .CollisionCause = Built.error()});
			}
			if (CollisionMode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0)
				Candidate.SimpleCollision = std::move(Geometry);
			else
				Candidate.ComplexCollision = std::move(Geometry);
			Candidate.CollisionPayloadBytes = CollisionBytes.size();
			Candidate.bHasCollision = true;
		}

		FStaticMeshPayloadData Payload;
		FCanonicalMemoryReader PayloadAr(RenderBytes, EArchivePurpose::CookedPayload, {.Target = ArchiveTarget});
		Payload.Serialize(PayloadAr);
		if (PayloadAr.IsError() || !RequireArchiveEnd(PayloadAr))
			return ArchiveFailure(ECookedMeshProductError::RenderArchive, PayloadAr, RenderBytes.size());
		if (Payload.MaterialSlotCount != MaterialSlots.size())
			return std::unexpected(FCookedMeshProductError{.Code = ECookedMeshProductError::MaterialSlotCount, .Actual = Payload.MaterialSlotCount, .Expected = MaterialSlots.size()});
		if (const auto Result = StaticMeshPrivate::TryMakeStaticMeshRenderData(Payload, Candidate.RenderData); !Result)
		{
			return std::unexpected(FCookedMeshProductError{.Code = ECookedMeshProductError::RenderConstruction, .RenderCause = Result.error()});
		}
		if (const auto Result = RestoreStaticMeshRuntimeMetadata(MaterialSlots, *Candidate.RenderData); !Result)
			return Result;
		Candidate.RenderData->RecalculateBounds();
		OutProduct = std::move(Candidate);
		return {};
	}
	auto DecodeStaticMeshCookedProduct(FByteView RenderBytes, FByteView CollisionBytes, std::span<const FMeshMaterialSlotDefinition> MaterialSlots, EBodySetupCollisionSourceMode CollisionMode, FStaticMeshCookedProduct& OutProduct, ECookTargetPlatform TargetPlatform) -> bool
	{
		if (const auto Result = AssetPrivate::TryDecodeStaticMeshCookedProduct(RenderBytes, CollisionBytes, MaterialSlots, CollisionMode, OutProduct, TargetPlatform); !Result)
		{
			DURIN_ERROR_CATEGORY("CookedMesh", "{}", AssetPrivate::FormatCookedMeshProductError(Result.error()));
			return false;
		}
		return true;
	}
}
