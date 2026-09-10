#include "Asset/OfflinePreparation.h"
#include "Logging/LogMacros.h"
#include "StaticMesh/StaticMesh.h"

#include "DObject/Package.h"

#include "Asset/AssetCook.h"
#include "Asset/CookDependencies.h"
#include "Asset/CookedMeshProducts.h"
#include "Asset/CookedMeshLoadManager.h"
#include "DObject/Property.h"
#include "Hash/XxHash.h"
#include "Physics/BodySetup.h"
#include "Serialization/Archive.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshCompilation.h"
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
			std::unique_ptr<FStaticMeshAuthoredCandidate> Candidate;
			const FStaticMeshRenderData* Projection = RenderData.get();
			std::string Error;
			if (Source.IsValid())
			{
				auto Request = MakeStaticMeshAuthoredBuildRequest(Source, CaptureStaticMeshReconciliation(*this));
				Request.bPersistDerivedData = false;
				if (!BuildStaticMeshAuthoredCandidate(std::move(Request), Candidate, Error))
				{
					Ar.Fail(EArchiveFailureCode::InvalidData, std::move(Error));
					return;
				}
				Projection = Candidate->GetRenderData();
			}
			if (!Projection)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"StaticMesh cooked render data is unavailable.");
				return;
			}
			FStaticMeshPayloadData Payload;
			FByteBuffer RenderBytes;
			if (!MakeStaticMeshPayloadData(*Projection, Payload, Error)
				|| !ValidateStaticMeshMaterialSlotMapping(Payload, MaterialSlots, Error))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, std::move(Error));
				return;
			}
			FCanonicalMemoryWriter RenderWriter(RenderBytes, EArchivePurpose::CookedPayload, {.Target = {"Win64", "Game"}});
			Payload.Serialize(RenderWriter);
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
				if (Candidate)
				{
					Simple = Candidate->GetCollision().Simple;
					Complex = Candidate->GetCollision().Complex;
				}
				else if (!BuildCollisionCandidate(*Projection, BodySetup->GetCollisionSourceMode(),
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
					CollisionBytes, EArchivePurpose::CookedPayload, {.Target = {"Win64", "Game"}});
				CollisionPayload.Serialize(CollisionWriter);
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

	auto DStaticMesh::PostLoad() -> void
	{
		std::string Error;
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (CookedRenderData.GetMetadata().LogicalSize == 0)
			{
				Error = std::format(
					"Cooked static mesh '{}': required RenderData field is missing.",
					GetObjectPath());
				DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
				return;
			}
			RenderData.reset();
			return;
		}
		if (MaterialSlots.size() > MaximumMeshMaterialSlots)
		{
			Error = "Static mesh material-slot count is outside the supported range.";
			DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
			return;
		}
		std::unordered_set<FName> SlotNames;
		for (const FMeshMaterialSlotDefinition& Slot : MaterialSlots)
		{
			if (Slot.Name.IsNone() || !SlotNames.insert(Slot.Name).second)
			{
				Error = "Static mesh material-slot names must be non-None and unique.";
				DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
				return;
			}
		}
		const DAssetImportData* ImportData = GetAssetImportData();
		const FSourceFile* SourceFile = ImportData
			? ImportData->GetSourceData().FindByRole("source") : nullptr;
		if (!GetSource().IsValid() && !SourceFile)
		{
			return;
		}
		if (!GetSource().IsValid())
		{
			Error = "StaticMesh canonical imported geometry is missing or invalid.";
			DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
			return;
		}
		// Cook serialization builds a detached candidate synchronously from this source.
		// Offline preparation builds directly instead of scheduling editor compilation.
		if (FScopedOfflinePreparation::IsActive())
		{
			return;
		}
		if (CanJoinStaticMeshCompilation(*this, Source)) return;
		if (!SubmitStaticMeshCompilation(*this, {.Source = Source, .bMarkPackageDirty = false}, Error))
		{
			DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
			return;
		}
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
		auto Read = CookedRenderData.AcquireRead();
		if (!Read) return FailCooked(Read.Error.Message);
		const FByteView Bytes = Read.Lock.GetBytes();
		FBulkDataReadResult CollisionRead;
		FByteView CollisionBytes;
		if (bRequiresCollision)
		{
			CollisionRead = CookedCollisionData.AcquireRead();
			if (!CollisionRead) return FailCooked(CollisionRead.Error.Message);
			CollisionBytes = CollisionRead.Lock.GetBytes();
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
			return FailCooked(std::move(ProductError.Message));
		}
		CollisionRead.Lock.Reset();
		Read.Lock.Reset();

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
		if (!RenderData && !Source.IsValid())
		{
			OutError = std::format("Static mesh '{}' has no render data to cook.", GetObjectPath());
			return false;
		}

		return Context.AddPackage(
			std::string(VirtualPackagePath), GetPackage(), &OutError);
	}

}
