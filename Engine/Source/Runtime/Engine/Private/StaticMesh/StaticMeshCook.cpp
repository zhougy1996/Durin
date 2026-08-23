#include "StaticMesh/StaticMesh.h"

#include "AssetCook.h"
#include "DObject/Property.h"
#include "Physics/BodySetup.h"
#include "Serialization/Archive.h"
#include "StaticMesh/StaticMeshAuthoring.h"
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
	auto DStaticMesh::PostLoad(std::string& OutError) -> bool
	{
		DerivedDataDiagnostic = {};
		if (Asset::GetAssetRuntimeConfiguration().RequiresCookedPayload())
			return LoadCookedRenderData(OutError);
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
		if (!SourceImportData.HasSource())
		{
			OutError.clear();
			return true;
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

		if (CookedPayload.PayloadId != StaticMeshPrimaryCookedPayloadId
			|| CookedPayload.LocationKind != static_cast<uint32>(Asset::ECookedPayloadLocationKind::PackageCompanion)
			|| CookedPayload.PayloadSchemaVersion != StaticMeshPayloadSchemaVersion
			|| CookedPayload.TargetPlatform != static_cast<uint32>(Asset::ECookTargetPlatform::Win64)
			|| CookedPayload.TargetProfile != static_cast<uint32>(Asset::ECookTargetProfile::Game)
			|| CookedPayload.CompressionMethod != static_cast<uint32>(Asset::ECookedPayloadCompression::None))
		{
			return FailCooked("required DMSH descriptor is missing or incompatible.");
		}
		const bool bRequiresCollision = BodySetup
			&& BodySetup->GetCollisionSourceMode() != EBodySetupCollisionSourceMode::None;
		const Asset::FCookedPayloadDescriptor* CollisionDescriptor = bRequiresCollision
			? &BodySetup->GetCookedCollisionPayloadDescriptor() : nullptr;
		if (bRequiresCollision
			&& (CollisionDescriptor->PayloadId != StaticMeshCollisionCookedPayloadId
				|| CollisionDescriptor->LocationKind != static_cast<uint32>(Asset::ECookedPayloadLocationKind::PackageCompanion)
				|| CollisionDescriptor->PayloadSchemaVersion != StaticMeshCollisionPayloadSchemaVersion
				|| CollisionDescriptor->TargetPlatform != static_cast<uint32>(Asset::ECookTargetPlatform::Win64)
				|| CollisionDescriptor->TargetProfile != static_cast<uint32>(Asset::ECookTargetProfile::Game)
				|| CollisionDescriptor->CompressionMethod != static_cast<uint32>(Asset::ECookedPayloadCompression::None)))
		{
			return FailCooked("required DCOL descriptor is missing or incompatible.");
		}

		const Asset::FAssetRuntimeConfiguration& LoadContext =
			Asset::GetAssetRuntimeConfiguration();
		if (!GetPackage())
			return FailCooked("package companion path could not be resolved.");
		Asset::FCookedPackagePayload LoadedPayload;
		if (!Asset::LoadCookedPackagePayload(
			LoadContext,
			GetPackage()->GetPackagePath(),
			CookedPayload,
			Asset::ECookTargetPlatform::Win64,
			Asset::ECookTargetProfile::Game,
			LoadedPayload,
			&OutError))
		{
			return FailCooked(OutError);
		}
		const std::span<const std::byte> Bytes = LoadedPayload.Payload;
		FCollisionGeometryRef CookedSimple;
		FCollisionGeometryRef CookedComplex;
		if (bRequiresCollision)
		{
			std::span<const std::byte> CollisionBytes;
			if (!Asset::ResolveCookedPayload(
				LoadedPayload.Container, *CollisionDescriptor, CollisionBytes, &OutError))
				return FailCooked(OutError);
			FStaticMeshCollisionPayloadData CollisionPayload;
			FCanonicalMemoryReader CollisionAr(
				CollisionBytes, EArchivePurpose::CookedPayload);
			CollisionPayload.Serialize(
				CollisionAr, EStaticMeshTargetPlatform::Win64);
			if (CollisionAr.HasError())
				return FailCooked(CollisionAr.GetFailure()->Message);
			if (CollisionPayload.SourceMode != BodySetup->GetCollisionSourceMode()
				|| CollisionPayload.QueryPolicy != BodySetup->GetCollisionQueryPolicy())
				return FailCooked("DCOL policy does not match its cooked BodySetup metadata.");
			FCollisionGeometryRef Geometry;
			if (!MakeStaticMeshCollisionGeometry(CollisionPayload, Geometry, OutError))
				return FailCooked(OutError);
			if (CollisionPayload.SourceMode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0)
				CookedSimple = Geometry;
			else
				CookedComplex = Geometry;
		}

		FStaticMeshPayloadData Payload;
		std::unique_ptr<FStaticMeshRenderData> CandidateRenderData;
		FCanonicalMemoryReader PayloadAr(Bytes, EArchivePurpose::CookedPayload);
		Payload.Serialize(PayloadAr, EStaticMeshTargetPlatform::Win64);
		if (PayloadAr.HasError())
			return FailCooked(PayloadAr.GetFailure()->Message);
		if (!ValidateStaticMeshMaterialSlotMapping(Payload, MaterialSlots, OutError)
			|| !MakeStaticMeshRenderData(Payload, CandidateRenderData, OutError)
			|| !RestoreStaticMeshRuntimeMetadata(MaterialSlots, *CandidateRenderData, OutError))
		{
			return FailCooked(OutError);
		}

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
				{}, "Loaded immutable collision from the cooked DCOL companion payload.",
				CollisionDescriptor->UncompressedSize);
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

		FStaticMeshPayloadData Payload;
		std::vector<std::byte> PayloadBytes;
		if (!MakeStaticMeshPayloadData(*RenderData, Payload, OutError)
			|| !ValidateStaticMeshMaterialSlotMapping(Payload, MaterialSlots, OutError))
		{
			OutError = std::format("Failed to cook static mesh '{}': {}", GetObjectPath(), OutError);
			return false;
		}
		FCanonicalMemoryWriter PayloadAr(
			PayloadBytes, EArchivePurpose::CookedPayload);
		Payload.Serialize(PayloadAr, EStaticMeshTargetPlatform::Win64);
		if (PayloadAr.HasError())
		{
			OutError = std::format("Failed to cook static mesh '{}': {}",
				GetObjectPath(), PayloadAr.GetFailure()->Message);
			return false;
		}

		Asset::FCookedBulkPayload BulkPayload{
			.PayloadId = StaticMeshPrimaryCookedPayloadId,
			.Flags = 1,
			.PayloadSchemaVersion = StaticMeshPayloadSchemaVersion,
			.Compression = Asset::ECookedPayloadCompression::None,
			.Alignment = StaticMeshPayloadAlignment,
			.Bytes = std::move(PayloadBytes)};
		std::vector<Asset::FCookedBulkPayload> BulkPayloads;
		BulkPayloads.emplace_back(std::move(BulkPayload));
		const bool bHasAuthoredCollision = BodySetup
			&& BodySetup->GetCollisionSourceMode() != EBodySetupCollisionSourceMode::None;
		if (bHasAuthoredCollision)
		{
			FCollisionGeometryRef Simple;
			FCollisionGeometryRef Complex;
			EBodySetupCollisionBuildStatus BuildStatus;
			std::string DerivedDataKey;
			std::string Diagnostic;
			uint64 CollisionPayloadBytes = 0;
			if (!BuildCollisionCandidate(
				*RenderData, BodySetup->GetCollisionSourceMode(),
				BodySetup->GetCollisionQueryPolicy(), Simple, Complex,
				BuildStatus, DerivedDataKey, Diagnostic, CollisionPayloadBytes, OutError))
			{
				OutError = std::format("Failed to cook static-mesh collision '{}': {}", GetObjectPath(), OutError);
				return false;
			}
			const FCollisionGeometryRef& CollisionGeometry =
				BodySetup->GetCollisionSourceMode() == EBodySetupCollisionSourceMode::ConvexHullFromLOD0
					? Simple : Complex;
			if (!CollisionGeometry)
			{
				OutError = "The detached collision build did not produce its required geometry.";
				return false;
			}
			FStaticMeshCollisionPayloadData CollisionPayload;
			std::vector<std::byte> CollisionBytes;
			if (!MakeStaticMeshCollisionPayloadData(
				CollisionGeometry, BodySetup->GetCollisionQueryPolicy(), CollisionPayload, OutError))
			{
				OutError = std::format("Failed to encode static-mesh collision '{}': {}", GetObjectPath(), OutError);
				return false;
			}
			FCanonicalMemoryWriter CollisionAr(
				CollisionBytes, EArchivePurpose::CookedPayload);
			CollisionPayload.Serialize(
				CollisionAr, EStaticMeshTargetPlatform::Win64);
			if (CollisionAr.HasError())
			{
				OutError = std::format("Failed to encode static-mesh collision '{}': {}",
					GetObjectPath(), CollisionAr.GetFailure()->Message);
				return false;
			}
			BulkPayloads.push_back({
				.PayloadId = StaticMeshCollisionCookedPayloadId,
				.Flags = 1,
				.PayloadSchemaVersion = StaticMeshCollisionPayloadSchemaVersion,
				.Compression = Asset::ECookedPayloadCompression::None,
				.Alignment = StaticMeshCollisionPayloadAlignment,
				.Bytes = std::move(CollisionBytes)});
		}
		const Asset::FAssetPackageSerializationOptions CookPackageOptions =
			Context.MakePackageSerializationOptions();

		return Context.AddPackage(
			std::string(VirtualPackagePath),
			std::move(BulkPayloads),
			[this, CookPackageOptions](
				std::span<const Asset::FCookedPayloadDescriptor> Descriptors,
				std::vector<std::byte>& OutPackageBytes,
				std::string* Error) {
				const auto RenderDescriptor = std::ranges::find(
					Descriptors, StaticMeshPrimaryCookedPayloadId,
					&Asset::FCookedPayloadDescriptor::PayloadId);
				const auto CollisionDescriptor = std::ranges::find(
					Descriptors, StaticMeshCollisionCookedPayloadId,
					&Asset::FCookedPayloadDescriptor::PayloadId);
				const bool bRequiresCollision = BodySetup
					&& BodySetup->GetCollisionSourceMode() != EBodySetupCollisionSourceMode::None;
				if (RenderDescriptor == Descriptors.end()
					|| (bRequiresCollision != (CollisionDescriptor != Descriptors.end()))
					|| Descriptors.size() != (bRequiresCollision ? 2u : 1u))
				{
					if (Error) *Error = "Static-mesh cook did not produce its exact required descriptor set.";
					return false;
				}

				FProperty* RenderProperty = GetClass()->FindPropertyByName("CookedPayload");
				if (!RenderProperty)
				{
					if (Error) *Error = "StaticMesh CookedPayload reflection is unavailable.";
					return false;
				}
				auto Overrides = std::make_shared<Asset::FObjectSaveOverrides>();
				std::string OverrideError;
				if (!Overrides->AddPropertyValue(
					*this, *RenderProperty, *RenderDescriptor, &OverrideError))
				{
					if (Error) *Error = OverrideError;
					return false;
				}
				if (BodySetup)
				{
					FProperty* CollisionProperty =
						BodySetup->GetClass()->FindPropertyByName("CookedCollisionPayload");
					const Asset::FCookedPayloadDescriptor EffectiveCollision = bRequiresCollision
						? *CollisionDescriptor : Asset::FCookedPayloadDescriptor{};
					if (!CollisionProperty || !Overrides->AddPropertyValue(
						*BodySetup, *CollisionProperty, EffectiveCollision, &OverrideError))
					{
						if (Error) *Error = CollisionProperty
							? OverrideError
							: "BodySetup CookedCollisionPayload reflection is unavailable.";
						return false;
					}
				}
				Asset::FAssetPackageSerializationOptions SerializationOptions = CookPackageOptions;
				SerializationOptions.SaveOverrides = std::move(Overrides);
				const Asset::FAssetResult Result = Asset::SerializeAssetPackageBytes(
					GetPackage(), OutPackageBytes, SerializationOptions);
				if (!Result)
				{
					if (Error) *Error = Result.Message;
					return false;
				}
				return true;
			},
			&OutError);
	}

}
