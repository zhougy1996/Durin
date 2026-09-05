#include "StaticMesh/StaticMesh.h"

#include "DObject/Package.h"

#include "Asset/AssetCook.h"
#include "Asset/CookedMeshProducts.h"
#include "Asset/CookedMeshLoadManager.h"
#include "DObject/Property.h"
#include "Hash/XxHash.h"
#include "Physics/BodySetup.h"
#include "Serialization/Archive.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshRenderStateRecreateContext.h"

namespace Durin
{
	namespace
	{
		struct FStaticMeshManagerProduct final
			: ICookedMeshDetachedProduct
		{
			FStaticMeshCookedProduct Product;
		};

		auto BuildStaticCookedMetadataIdentity(const DStaticMesh& Mesh) -> uint64
		{
			FXxHash64Builder Builder;
			auto AddBulk = [&Builder](const FBulkData& Bulk) {
				const FBulkDataMetadata Metadata = Bulk.GetMetadata();
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
		FBulkData RenderProjection;
		FBulkData CollisionProjection;
		FBulkData* RenderField = &CookedRenderData;
		FBulkData* CollisionField = &CookedCollisionData;
		if (Ar.IsSaving())
		{
			if (!RenderData)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"StaticMesh cooked render data is unavailable.");
				return;
			}
			FStaticMeshPayloadData Payload;
			FByteBuffer RenderBytes;
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
				|| !FBulkData::TryCreateDetached(RenderBytes, RenderProjection, &Error))
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
				if (!BuildCollisionCandidate(*RenderData, BodySetup->GetCollisionSourceMode(),
					BodySetup->GetCollisionQueryPolicy(), Simple, Complex, Error))
				{
					Ar.Fail(EArchiveFailureCode::InvalidData, std::move(Error));
					return;
				}
				const FCollisionGeometryRef& Geometry =
					BodySetup->GetCollisionSourceMode()
						== EBodySetupCollisionSourceMode::ConvexHullFromLOD0 ? Simple : Complex;
				FStaticMeshCollisionPayloadData CollisionPayload;
				FByteBuffer CollisionBytes;
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
				if (CollisionWriter.HasError() || !FBulkData::TryCreateDetached(
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
			CollisionField->Serialize(Ar, {.Alignment = EditorBulkDataExternalAlignment,
				.StoragePolicy = EArchiveBulkDataStoragePolicy::AllowExternal});
		}
		{
			auto Field = EnterArchiveField(Ar, {FName("Durin::DStaticMesh"),
				FName("RenderData"), FArchiveLogicalTypeDescriptor::BulkData()});
			RenderField->Serialize(Ar, {.Alignment = EditorBulkDataExternalAlignment,
				.StoragePolicy = EArchiveBulkDataStoragePolicy::AllowExternal});
		}
	}

	auto DStaticMesh::PostLoad(std::string& OutError) -> bool
	{
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (CookedRenderData.GetMetadata().LogicalSize == 0)
			{
				OutError = std::format(
					"Cooked static mesh '{}': required RenderData field is missing.",
					GetObjectPath());
				return false;
			}
			RenderData.reset();
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
		FStaticMeshBuildResult Product;
		if (!BuildStaticMeshDerivedData({
			.Reconciliation = CaptureStaticMeshReconciliation(*this),
			.ImportedData = ImportedData}, Product, OutError)) return false;
		return ApplyStaticMeshBuildResult(*this, std::move(Product), OutError, false);
	}
	auto DStaticMesh::LoadCookedRenderData(std::string& OutError) -> bool
	{
		auto FailCooked = [&](std::string Message) {
			OutError = std::format(
				"Cooked static mesh '{}': {}", GetObjectPath(), Message);
			return false;
		};

		const bool bRequiresCollision = BodySetup
			&& BodySetup->GetCollisionSourceMode() != EBodySetupCollisionSourceMode::None;
		FByteView Bytes;
		if (!CookedRenderData.LockReadOnly(Bytes, &OutError))
			return FailCooked(OutError);
		FByteView CollisionBytes;
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
			const bool bPublished = BodySetup->SetCollisionGeometry(
				Product.SimpleCollision, Product.ComplexCollision);
			check(bPublished);
		}
		OutError.clear();
		return true;
	}

	auto DStaticMesh::SubmitCookedRenderDataRequest(bool bInitializeResources) -> bool
	{
		FCookedMeshLoadManager* Manager =
			GetCookedMeshLoadManager();
		if (!Manager || RenderData
			|| !GetAssetRuntimeConfiguration().RequiresCookedPayload()
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

		FCookedMeshLoadRequest Request{
			.Identity = {
				.Owner = MakeObjectHandle(this),
				.Family = ECookedMeshFamily::StaticMesh,
				.LoadGeneration = Generation,
				.ResourceRevision = ResourceRevision,
				.MetadataIdentity = MetadataIdentity},
			.Fields = {CookedRenderData},
			.Worker = [SlotSnapshot = std::move(SlotSnapshot), CollisionMode,
				CollisionPolicy, bRequiresCollision](
				std::span<const FSharedByteBuffer> Buffers,
				const FTaskCancellationToken& Cancellation)
				-> FCookedMeshWorkerResult {
				if (Cancellation.IsCancellationRequested()) return {};
				if (Buffers.size() != (bRequiresCollision ? 2u : 1u))
					return {.Message = "StaticMesh cooked field count is invalid."};
				auto Result = std::make_unique<FStaticMeshManagerProduct>();
				FCookedMeshProductError Error;
				const FByteView CollisionBytes = bRequiresCollision
					? Buffers[1].GetBytes() : FByteView{};
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
				const FCookedMeshLoadIdentity& Identity) {
				const auto* Mesh = Cast<DStaticMesh>(&Owner);
				return Mesh
					&& Mesh->CookedLoadGeneration.load(std::memory_order_acquire)
						== Identity.LoadGeneration
					&& Mesh->GetRenderResourceStatus().Revision
						== Identity.ResourceRevision
					&& BuildStaticCookedMetadataIdentity(*Mesh)
						== Identity.MetadataIdentity;
			},
			.Publish = [bInitializeResources](DObject& Owner,
				const FCookedMeshLoadIdentity&,
				std::unique_ptr<ICookedMeshDetachedProduct> BaseProduct,
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
						|| !Mesh->BodySetup->SetCollisionGeometry(
							Product.SimpleCollision, Product.ComplexCollision))
					{
						OutError = "StaticMesh cooked collision publication failed.";
						return false;
					}
				}
				Mesh->CookedLoadPhase.store(
					ECookedMeshCpuPhase::CpuReady, std::memory_order_release);
				if (bInitializeResources) Mesh->InitResources();
				OutError.clear();
				return true;
			},
			.OnTerminal = [](DObject& Owner,
				const FCookedMeshLoadIdentity&,
				ECookedMeshTerminalState Terminal,
				std::string_view) {
				auto* Mesh = Cast<DStaticMesh>(&Owner);
				if (!Mesh) return;
				const bool bFailed = Terminal == ECookedMeshTerminalState::Failed
					|| Terminal == ECookedMeshTerminalState::Rejected;
				Mesh->CookedLoadPhase.store(bFailed
					? ECookedMeshCpuPhase::Failed
					: ECookedMeshCpuPhase::Cancelled, std::memory_order_release);
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
		FCookContext& Context,
		std::string_view VirtualPackagePath,
		std::string& OutError) -> bool
	{
		if (Context.GetTargetPlatform() != ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != ECookTargetProfile::Game)
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
