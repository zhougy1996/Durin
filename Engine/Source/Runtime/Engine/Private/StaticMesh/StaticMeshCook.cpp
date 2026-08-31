#include "StaticMesh/StaticMesh.h"

#include "DObject/Package.h"

#include "Asset/AssetCook.h"
#include "Asset/CookedMeshProducts.h"
#include "Asset/CookedMeshLoadManager.h"
#include "DObject/Property.h"
#include "Hash/XxHash.h"
#include "Physics/BodySetup.h"
#include "Serialization/Archive.h"
#include "StaticMesh/StaticMeshPostLoad.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshRenderStateRecreateContext.h"

namespace Durin
{
	namespace
	{
		struct FStaticMeshManagerProduct final
			: Asset::ICookedMeshDetachedProduct
		{
			FStaticMeshCookedProduct Product;
		};

		auto BuildStaticCookedMetadataIdentity(const DStaticMesh& Mesh) -> uint64
		{
			FXxHash64Builder Builder;
			auto AddBulk = [&Builder](const Asset::FBulkData& Bulk) {
				const Asset::FBulkDataMetadata Metadata = Bulk.GetMetadata();
				Builder.UpdateValue(Metadata.LogicalSize);
				Builder.UpdateValue(Metadata.Range.SegmentOffset);
				Builder.UpdateValue(Metadata.Range.StoredSize);
				Builder.UpdateValue(Metadata.Range.StorageFlags);
				Builder.UpdateValue(Metadata.Range.Alignment);
				const uintptr_t Resource = reinterpret_cast<uintptr_t>(
					Metadata.Range.Resource.get());
				Builder.UpdateValue(Resource);
			};
			AddBulk(Mesh.GetCookedRenderData());
			AddBulk(Mesh.GetCookedCollisionData());
			const DBodySetup* BodySetup = Mesh.GetBodySetup();
			const EBodySetupCollisionSourceMode Mode = BodySetup
				? BodySetup->GetCollisionSourceMode()
				: EBodySetupCollisionSourceMode::None;
			const EBodySetupCollisionQueryPolicy Policy = BodySetup
				? BodySetup->GetCollisionQueryPolicy()
				: EBodySetupCollisionQueryPolicy::SimpleAndComplex;
			Builder.UpdateValue(Mode);
			Builder.UpdateValue(Policy);
			for (const FMeshMaterialSlotDefinition& Slot : Mesh.GetMaterialSlots())
			{
				Builder.Update(Slot.Name.ToString());
				Builder.UpdateValue(Slot.SourceMaterialIndex);
			}
			return Builder.Finalize().HashValue;
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
			FByteArray RenderBytes;
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
				FByteArray CollisionBytes;
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
		std::span<const std::byte> CollisionBytes;
		if (bRequiresCollision)
		{
			if (!CookedCollisionData.LockReadOnly(CollisionBytes, &OutError))
			{
				CookedRenderData.UnlockReadOnly();
				return FailCooked(OutError);
			}
		}

		FStaticMeshCookedProduct Product;
		FCookedMeshProductError ProductError;
		const EBodySetupCollisionSourceMode CollisionMode = bRequiresCollision
			? BodySetup->GetCollisionSourceMode()
			: EBodySetupCollisionSourceMode::None;
		const EBodySetupCollisionQueryPolicy CollisionPolicy = BodySetup
			? BodySetup->GetCollisionQueryPolicy()
			: EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		if (!DecodeStaticMeshCookedProduct(Bytes, CollisionBytes, MaterialSlots,
			CollisionMode, CollisionPolicy, Product, ProductError))
		{
			if (bRequiresCollision) CookedCollisionData.UnlockReadOnly();
			CookedRenderData.UnlockReadOnly();
			return FailCooked(std::move(ProductError.Message));
		}
		if (bRequiresCollision && !CookedCollisionData.UnlockReadOnly(&OutError))
		{
			CookedRenderData.UnlockReadOnly();
			return FailCooked(OutError);
		}
		if (!CookedRenderData.UnlockReadOnly(&OutError)) return FailCooked(OutError);

		if (!CommitRenderDataCandidate(
			std::move(Product.RenderData), nullptr, OutError, false))
		{
			return FailCooked(OutError);
		}
		if (bRequiresCollision)
		{
			const bool bPublished = BodySetup->PublishCollisionGeometry(
				Product.SimpleCollision, Product.ComplexCollision,
				EBodySetupCollisionBuildStatus::CookedLoaded,
				{}, "Loaded immutable collision from the cooked DCOL package field.",
				Product.CollisionPayloadBytes);
			check(bPublished);
		}
		DerivedDataDiagnostic.Status = EStaticMeshDerivedDataStatus::CookedLoaded;
		DerivedDataDiagnostic.Message = std::format(
			"Loaded cooked static-mesh payload for '{}'.", GetObjectPath());
		OutError.clear();
		return true;
	}

	auto DStaticMesh::SubmitCookedRenderDataRequest() -> bool
	{
		Asset::FCookedMeshLoadManager* Manager =
			Asset::GetCookedMeshLoadManager();
		if (!Manager || RenderData
			|| !Asset::GetAssetRuntimeConfiguration().RequiresCookedPayload()
			|| CookedRenderData.GetMetadata().LogicalSize == 0)
		{
			return false;
		}

		const DBodySetup* CurrentBodySetup = BodySetup.Get();
		const EBodySetupCollisionSourceMode CollisionMode = CurrentBodySetup
			? CurrentBodySetup->GetCollisionSourceMode()
			: EBodySetupCollisionSourceMode::None;
		const EBodySetupCollisionQueryPolicy CollisionPolicy = CurrentBodySetup
			? CurrentBodySetup->GetCollisionQueryPolicy()
			: EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		const bool bRequiresCollision =
			CollisionMode != EBodySetupCollisionSourceMode::None;
		const uint64 Generation =
			CookedLoadGeneration.load(std::memory_order_acquire);
		const uint64 ResourceRevision = GetRenderResourceStatus().Revision;
		const uint64 MetadataIdentity = BuildStaticCookedMetadataIdentity(*this);
		std::vector<FMeshMaterialSlotDefinition> SlotSnapshot = MaterialSlots;

		Asset::FCookedMeshLoadRequest Request{
			.Identity = {
				.Owner = MakeObjectHandle(this),
				.Family = Asset::ECookedMeshFamily::StaticMesh,
				.LoadGeneration = Generation,
				.ResourceRevision = ResourceRevision,
				.MetadataIdentity = MetadataIdentity},
			.Fields = {CookedRenderData},
			.Worker = [SlotSnapshot = std::move(SlotSnapshot), CollisionMode,
				CollisionPolicy, bRequiresCollision](
				std::span<const FSharedByteBuffer> Buffers,
				const FTaskCancellationToken& Cancellation)
				-> Asset::FCookedMeshWorkerResult {
				if (Cancellation.IsCancellationRequested()) return {};
				if (Buffers.size() != (bRequiresCollision ? 2u : 1u))
					return {.Message = "StaticMesh cooked field count is invalid."};
				auto Result = std::make_unique<FStaticMeshManagerProduct>();
				FCookedMeshProductError Error;
				const std::span<const std::byte> CollisionBytes = bRequiresCollision
					? Buffers[1].GetBytes() : std::span<const std::byte>{};
				if (!DecodeStaticMeshCookedProduct(Buffers[0].GetBytes(),
					CollisionBytes, SlotSnapshot, CollisionMode, CollisionPolicy,
					Result->Product, Error))
				{
					return {.Message = std::move(Error.Message)};
				}
				uint64 RetainedBytes = Buffers[0].GetSize();
				if (bRequiresCollision)
					RetainedBytes += Buffers[1].GetSize();
				if (Result->Product.SimpleCollision)
					RetainedBytes += Result->Product.SimpleCollision.GetRetainedBytes();
				if (Result->Product.ComplexCollision)
					RetainedBytes += Result->Product.ComplexCollision.GetRetainedBytes();
				return {.Product = std::move(Result),
					.RetainedBytes = std::max<uint64>(RetainedBytes, 1)};
			},
			.IsCurrent = [](const DObject& Owner,
				const Asset::FCookedMeshLoadIdentity& Identity) {
				const auto* Mesh = Cast<DStaticMesh>(&Owner);
				return Mesh
					&& Mesh->CookedLoadGeneration.load(std::memory_order_acquire)
						== Identity.LoadGeneration
					&& Mesh->GetRenderResourceStatus().Revision
						== Identity.ResourceRevision
					&& BuildStaticCookedMetadataIdentity(*Mesh)
						== Identity.MetadataIdentity;
			},
			.Publish = [](DObject& Owner,
				const Asset::FCookedMeshLoadIdentity&,
				std::unique_ptr<Asset::ICookedMeshDetachedProduct> BaseProduct,
				std::string& OutError) {
				auto* Mesh = Cast<DStaticMesh>(&Owner);
				auto* Typed = dynamic_cast<FStaticMeshManagerProduct*>(
					BaseProduct.get());
				if (!Mesh || !Typed)
				{
					OutError = "StaticMesh cooked publication product is invalid.";
					return false;
				}
				FStaticMeshRenderStateRecreateContext RecreateContext(Mesh);
				FStaticMeshCookedProduct Product = std::move(Typed->Product);
				if (!Mesh->CommitRenderDataCandidate(
					std::move(Product.RenderData), nullptr, OutError, false))
				{
					return false;
				}
				if (Product.bHasCollision)
				{
					if (!Mesh->BodySetup
						|| !Mesh->BodySetup->PublishCollisionGeometry(
							Product.SimpleCollision, Product.ComplexCollision,
							EBodySetupCollisionBuildStatus::CookedLoaded, {},
							"Loaded immutable collision from the cooked DCOL package field.",
							Product.CollisionPayloadBytes))
					{
						OutError = "StaticMesh cooked collision publication failed.";
						return false;
					}
				}
				Mesh->DerivedDataDiagnostic.Status =
					EStaticMeshDerivedDataStatus::CookedLoaded;
				Mesh->DerivedDataDiagnostic.Message = std::format(
					"Loaded cooked static-mesh payload for '{}'.",
					Mesh->GetObjectPath());
				Mesh->CookedLoadPhase.store(
					ECookedMeshCpuPhase::CpuReady, std::memory_order_release);
				Mesh->InitResources();
				OutError.clear();
				return true;
			},
			.OnTerminal = [](DObject& Owner,
				const Asset::FCookedMeshLoadIdentity&,
				Asset::ECookedMeshTerminalState Terminal,
				std::string_view Message) {
				auto* Mesh = Cast<DStaticMesh>(&Owner);
				if (!Mesh) return;
				const bool bFailed = Terminal == Asset::ECookedMeshTerminalState::Failed
					|| Terminal == Asset::ECookedMeshTerminalState::Rejected;
				Mesh->CookedLoadPhase.store(bFailed
					? ECookedMeshCpuPhase::Failed
					: ECookedMeshCpuPhase::Cancelled, std::memory_order_release);
				Mesh->DerivedDataDiagnostic.Status =
					EStaticMeshDerivedDataStatus::CookedFailure;
				Mesh->DerivedDataDiagnostic.Message = std::format(
					"Cooked static mesh '{}': {}", Mesh->GetObjectPath(),
					Message.empty() ? "asynchronous load terminated" : Message);
			}
		};
		if (bRequiresCollision)
			Request.Fields.push_back(CookedCollisionData);
		if (!Manager->Submit(std::move(Request))) return false;
		CookedLoadPhase.store(
			ECookedMeshCpuPhase::IoQueued, std::memory_order_release);
		return true;
	}

	auto DStaticMesh::ContributeToCook(
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
