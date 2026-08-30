#include "StaticMesh/StaticMesh.h"

#include "DObject/Package.h"

#include "AssetCook.h"
#include "DObject/Property.h"
#include "Physics/BodySetup.h"
#include "Serialization/Archive.h"
#include "StaticMesh/StaticMeshPostLoad.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace Durin
{
	namespace
	{
		auto RestoreStaticMeshRuntimeMetadata(
			const std::vector<FMeshMaterialSlotDefinition>& MaterialSlots,
			FStaticMeshRenderData& RenderData,
			std::string& OutError) -> bool
		{
			if (RenderData.MaterialSlots.size() != MaterialSlots.size())
			{
				OutError = "Cached static-mesh material slot count does not match asset metadata.";
				return false;
			}
			for (size_t SlotIndex = 0; SlotIndex < MaterialSlots.size(); ++SlotIndex)
			{
				const FMeshMaterialSlotDefinition& Definition = MaterialSlots[SlotIndex];
				FStaticMeshMaterialSlot& Slot = RenderData.MaterialSlots[SlotIndex];
				// Editable asset metadata is authoritative; the cached payload contributes
				// only the compatible stable slot count and ordering.
				Slot.Name = Definition.Name.ToString();
				Slot.SourceMaterialIndex = Definition.SourceMaterialIndex;
			}
			for (size_t LODIndex = 0; LODIndex < RenderData.LODResources.size(); ++LODIndex)
			{
				auto& Sections = RenderData.LODResources[LODIndex].Sections;
				for (size_t SectionIndex = 0; SectionIndex < Sections.size(); ++SectionIndex)
					Sections[SectionIndex].Name = std::format("LOD{}_Section{}", LODIndex, SectionIndex);
			}
			return true;
		}

		auto ValidateStaticMeshMaterialSlotMapping(
			const FStaticMeshPayloadData& Payload,
			const std::vector<FMeshMaterialSlotDefinition>& MaterialSlots,
			std::string& OutError) -> bool
		{
			if (Payload.MaterialSlotCount != MaterialSlots.size())
			{
				OutError = "Static-mesh payload material slot count does not match package metadata.";
				return false;
			}
			return true;
		}




	}

	auto DStaticMesh::SerializeCooked(FArchive& Ar) -> void
	{
		Super::SerializeCooked(Ar);
		if (Ar.GetTarget().Platform != "Win64" || Ar.GetTarget().Profile != "Game")
		{
			Ar.Fail(EArchiveFailureCode::InvalidData,
				"StaticMesh cooked platform data requires the Win64 Game target.");
			return;
		}
		Asset::FBulkData RenderProjection;
		Asset::FBulkData CollisionProjection;
		Asset::FBulkData* RenderField = &CookedRenderData;
		Asset::FBulkData* CollisionField = &CookedCollisionData;
		if (Ar.IsSaving())
		{
			if (!RenderData)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"StaticMesh cooked render data is unavailable.");
				return;
			}
			FStaticMeshPayloadData Payload;
			std::vector<std::byte> RenderBytes;
			std::string Error;
			if (!MakeStaticMeshPayloadData(*RenderData, Payload, Error)
				|| !ValidateStaticMeshMaterialSlotMapping(Payload, MaterialSlots, Error))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, std::move(Error));
				return;
			}
			FCanonicalMemoryWriter RenderWriter(RenderBytes, EArchivePurpose::CookedPayload);
			Payload.Serialize(RenderWriter, EStaticMeshTargetPlatform::Win64);
			if (RenderWriter.HasError()
				|| !Asset::FBulkData::TryCreateDetached(RenderBytes, RenderProjection, &Error))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, Error.empty()
					? RenderWriter.GetFailure()->Message : std::move(Error));
				return;
			}
			RenderField = &RenderProjection;

			if (BodySetup
				&& BodySetup->GetCollisionSourceMode() != EBodySetupCollisionSourceMode::None)
			{
				FCollisionGeometryRef Simple, Complex;
				EBodySetupCollisionBuildStatus BuildStatus;
				std::string Key, Diagnostic;
				uint64 CollisionPayloadBytes = 0;
				if (!BuildCollisionCandidate(*RenderData, BodySetup->GetCollisionSourceMode(),
					BodySetup->GetCollisionQueryPolicy(), Simple, Complex, BuildStatus, Key,
					Diagnostic, CollisionPayloadBytes, Error))
				{
					Ar.Fail(EArchiveFailureCode::InvalidData, std::move(Error));
					return;
				}
				const FCollisionGeometryRef& Geometry =
					BodySetup->GetCollisionSourceMode()
						== EBodySetupCollisionSourceMode::ConvexHullFromLOD0 ? Simple : Complex;
				FStaticMeshCollisionPayloadData CollisionPayload;
				std::vector<std::byte> CollisionBytes;
				if (!Geometry || !MakeStaticMeshCollisionPayloadData(
					Geometry, BodySetup->GetCollisionQueryPolicy(), CollisionPayload, Error))
				{
					Ar.Fail(EArchiveFailureCode::InvalidData, Error.empty()
						? "StaticMesh cooked collision data is unavailable." : std::move(Error));
					return;
				}
				FCanonicalMemoryWriter CollisionWriter(
					CollisionBytes, EArchivePurpose::CookedPayload);
				CollisionPayload.Serialize(CollisionWriter, EStaticMeshTargetPlatform::Win64);
				if (CollisionWriter.HasError() || !Asset::FBulkData::TryCreateDetached(
					CollisionBytes, CollisionProjection, &Error))
				{
					Ar.Fail(EArchiveFailureCode::InvalidData, Error.empty()
						? CollisionWriter.GetFailure()->Message : std::move(Error));
					return;
				}
				CollisionField = &CollisionProjection;
			}
		}
		{
			auto Field = EnterArchiveField(Ar, {FName("Durin::DStaticMesh"),
				FName("CollisionData"), FArchiveLogicalTypeDescriptor::BulkData()});
			CollisionField->Serialize(Ar, {.Alignment = Asset::EditorBulkDataExternalAlignment,
				.StoragePolicy = EArchiveBulkDataStoragePolicy::AllowExternal});
		}
		{
			auto Field = EnterArchiveField(Ar, {FName("Durin::DStaticMesh"),
				FName("RenderData"), FArchiveLogicalTypeDescriptor::BulkData()});
			RenderField->Serialize(Ar, {.Alignment = Asset::EditorBulkDataExternalAlignment,
				.StoragePolicy = EArchiveBulkDataStoragePolicy::AllowExternal});
		}
	}

	auto DStaticMesh::PostLoad(std::string& OutError) -> bool
	{
		DerivedDataDiagnostic = {};
		if (Asset::GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (CookedRenderData.GetMetadata().LogicalSize == 0)
			{
				OutError = std::format(
					"Cooked static mesh '{}': required RenderData field is missing.",
					GetObjectPath());
				return false;
			}
			RenderData.reset();
			DerivedDataDiagnostic.Status = EStaticMeshDerivedDataStatus::CookedLoaded;
			DerivedDataDiagnostic.Message = std::format(
				"Loaded cooked static-mesh metadata for '{}'.", GetObjectPath());
			OutError.clear();
			return true;
		}
		if (MaterialSlots.size() > MaximumMeshMaterialSlots)
		{
			OutError = "Static mesh material-slot count is outside the supported range.";
			return false;
		}
		std::unordered_set<FName> SlotNames;
		for (const FMeshMaterialSlotDefinition& Slot : MaterialSlots)
		{
			if (Slot.Name.IsNone() || !SlotNames.insert(Slot.Name).second)
			{
				OutError = "Static mesh material-slot names must be non-None and unique.";
				return false;
			}
		}
		const DAssetImportData* ImportData = GetAssetImportData();
		const FSourceFile* Source = ImportData
			? ImportData->GetSourceData().FindByRole("source") : nullptr;
		if (!GetImportedData().IsValid() && !Source)
		{
			OutError.clear();
			return true;
		}
		if (!GetImportedData().IsValid())
		{
			OutError = "StaticMesh canonical imported geometry is missing or invalid.";
			return false;
		}
		return InvokeStaticMeshPostLoadFeature(*this, DerivedDataDiagnostic, OutError);
	}
	auto DStaticMesh::LoadCookedRenderData(std::string& OutError) -> bool
	{
		auto FailCooked = [&](std::string Message) {
			DerivedDataDiagnostic.Status = EStaticMeshDerivedDataStatus::CookedFailure;
			DerivedDataDiagnostic.Message = std::format(
				"Cooked static mesh '{}': {}", GetObjectPath(), Message);
			OutError = DerivedDataDiagnostic.Message;
			return false;
		};

		const bool bRequiresCollision = BodySetup
			&& BodySetup->GetCollisionSourceMode() != EBodySetupCollisionSourceMode::None;
		std::span<const std::byte> Bytes;
		if (!CookedRenderData.LockReadOnly(Bytes, &OutError))
			return FailCooked(OutError);
		FCollisionGeometryRef CookedSimple;
		FCollisionGeometryRef CookedComplex;
		if (bRequiresCollision)
		{
			std::span<const std::byte> CollisionBytes;
			if (!CookedCollisionData.LockReadOnly(CollisionBytes, &OutError))
			{
				CookedRenderData.UnlockReadOnly();
				return FailCooked(OutError);
			}
			FStaticMeshCollisionPayloadData CollisionPayload;
			FCanonicalMemoryReader CollisionAr(
				CollisionBytes, EArchivePurpose::CookedPayload);
			CollisionPayload.Serialize(
				CollisionAr, EStaticMeshTargetPlatform::Win64);
			if (CollisionAr.HasError() || !RequireArchiveEnd(CollisionAr))
			{
				const std::string Error(CollisionAr.GetError());
				CookedCollisionData.UnlockReadOnly();
				CookedRenderData.UnlockReadOnly();
				return FailCooked(Error);
			}
			if (CollisionPayload.SourceMode != BodySetup->GetCollisionSourceMode()
				|| CollisionPayload.QueryPolicy != BodySetup->GetCollisionQueryPolicy())
			{
				CookedCollisionData.UnlockReadOnly();
				CookedRenderData.UnlockReadOnly();
				return FailCooked("DCOL policy does not match its cooked BodySetup metadata.");
			}
			FCollisionGeometryRef Geometry;
			if (!MakeStaticMeshCollisionGeometry(CollisionPayload, Geometry, OutError))
			{
				CookedCollisionData.UnlockReadOnly();
				CookedRenderData.UnlockReadOnly();
				return FailCooked(OutError);
			}
			if (CollisionPayload.SourceMode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0)
				CookedSimple = Geometry;
			else
				CookedComplex = Geometry;
			if (!CookedCollisionData.UnlockReadOnly(&OutError))
			{
				CookedRenderData.UnlockReadOnly();
				return FailCooked(OutError);
			}
		}

		FStaticMeshPayloadData Payload;
		std::unique_ptr<FStaticMeshRenderData> CandidateRenderData;
		FCanonicalMemoryReader PayloadAr(Bytes, EArchivePurpose::CookedPayload);
		Payload.Serialize(PayloadAr, EStaticMeshTargetPlatform::Win64);
		if (PayloadAr.HasError() || !RequireArchiveEnd(PayloadAr))
		{
			const std::string Error(PayloadAr.GetError());
			CookedRenderData.UnlockReadOnly();
			return FailCooked(Error);
		}
		if (!ValidateStaticMeshMaterialSlotMapping(Payload, MaterialSlots, OutError)
			|| !MakeStaticMeshRenderData(Payload, CandidateRenderData, OutError)
			|| !RestoreStaticMeshRuntimeMetadata(MaterialSlots, *CandidateRenderData, OutError))
		{
			CookedRenderData.UnlockReadOnly();
			return FailCooked(OutError);
		}
		if (!CookedRenderData.UnlockReadOnly(&OutError)) return FailCooked(OutError);

		if (!CommitRenderDataCandidate(
			std::move(CandidateRenderData), nullptr, OutError, false))
		{
			return FailCooked(OutError);
		}
		if (bRequiresCollision)
		{
			const bool bPublished = BodySetup->PublishCollisionGeometry(
				CookedSimple, CookedComplex,
				EBodySetupCollisionBuildStatus::CookedLoaded,
				{}, "Loaded immutable collision from the cooked DCOL package field.",
				CookedCollisionData.GetMetadata().LogicalSize);
			check(bPublished);
		}
		DerivedDataDiagnostic.Status = EStaticMeshDerivedDataStatus::CookedLoaded;
		DerivedDataDiagnostic.Message = std::format(
			"Loaded cooked static-mesh payload for '{}'.", GetObjectPath());
		OutError.clear();
		return true;
	}

	auto DStaticMesh::AddToCook(
		Asset::FCookContext& Context,
		std::string_view VirtualPackagePath,
		std::string& OutError) -> bool
	{
		if (Context.GetTargetPlatform() != Asset::ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != Asset::ECookTargetProfile::Game)
		{
			OutError = std::format(
				"Static mesh '{}' supports only the Win64 game cook target.", GetObjectPath());
			return false;
		}
		if (!RenderData && !PostLoad(OutError)) return false;
		if (!RenderData)
		{
			OutError = std::format("Static mesh '{}' has no render data to cook.", GetObjectPath());
			return false;
		}

		return Context.AddPackage(
			std::string(VirtualPackagePath), GetPackage(), &OutError);
	}

}
