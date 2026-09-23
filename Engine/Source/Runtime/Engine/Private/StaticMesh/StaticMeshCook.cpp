#include "Physics/PhysicsDerivedData.h"
#include "Physics/PhysicsCookHelper.h"
#include "Asset/OfflinePreparation.h"
#include "Logging/LogMacros.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshCustomVersion.h"

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

	auto DStaticMesh::Serialize(FArchive& Ar) -> void
	{
		if (!FStaticMeshSourceVersion::Serialize(Ar)) return;
		Super::Serialize(Ar);
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
			std::unique_ptr<FStaticMeshRenderData> DetachedRender;
			const FStaticMeshRenderData* Projection = RenderData.get();
			std::string Error;
			if (Source.IsValid())
			{
				FStaticMeshBuildRequest Request{.Reconciliation = CaptureStaticMeshReconciliation(*this), .Source = Source};
				Request.bPersistDerivedData = false;
				auto Built = BuildStaticMeshRenderData(std::move(Request));
				if (!Built)
				{
					Ar.Fail(EArchiveFailureCode::InvalidData, Built.error().ToString());
					return;
				}
				DetachedRender = std::move(*Built);
				Projection = DetachedRender.get();
			}
			if (!Projection)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"StaticMesh cooked render data is unavailable.");
				return;
			}
			FStaticMeshPayloadData Payload;
			FByteBuffer RenderBytes;
			if (const auto Result = MakeStaticMeshPayloadData(*Projection, Payload); !Result)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, FormatStaticMeshPayloadError(Result.error()));
				return;
			}
			if (!ValidateStaticMeshMaterialSlotMapping(Payload, MaterialSlots, Error))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, std::move(Error));
				return;
			}
			FCanonicalMemoryWriter RenderWriter(RenderBytes, EArchivePurpose::CookedPayload, {.Target = {"Win64", "Game"}});
			Payload.Serialize(RenderWriter);
			auto RenderBulk = RenderWriter.IsError() ? std::expected<FBulkData, FBulkDataError>{}
				: FBulkData::TryCreateDetached(RenderBytes);
			if (RenderWriter.IsError() || !RenderBulk)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, RenderWriter.IsError()
					? RenderWriter.GetFailure()->Message : FormatBulkDataError(RenderBulk.error()));
				return;
			}
			RenderProjection = std::move(*RenderBulk);
			RenderField = &RenderProjection;

			if (BodySetup
				&& BodySetup->GetCollisionSourceMode() != EBodySetupCollisionSourceMode::None)
			{
				FCollisionGeometryRef Simple, Complex;
				auto Data = GetPhysicsTriMeshData();
				if (!Data) { Ar.Fail(EArchiveFailureCode::InvalidData, Data.error().ToString()); return; }
				auto Collision = FPhysicsCookHelper::Cook(BodySetup->GetCookInfo(std::move(*Data), false));
				if (!Collision)
				{
					Ar.Fail(EArchiveFailureCode::InvalidData, Collision.error().ToString());
					return;
				}
				Simple = std::move(Collision->Simple);
				Complex = std::move(Collision->Complex);
				const FCollisionGeometryRef& Geometry =
					BodySetup->GetCollisionSourceMode()
						== EBodySetupCollisionSourceMode::ConvexHullFromLOD0 ? Simple : Complex;
				FPhysicsCollisionPayloadData CollisionPayload;
				FByteBuffer CollisionBytes;
				if (const auto Built = MakePhysicsCollisionPayloadData(
					Geometry, BodySetup->GetCollisionQueryPolicy(), CollisionPayload); !Built)
				{
					Ar.Fail(EArchiveFailureCode::InvalidData, FormatPhysicsCollisionPayloadError(Built.error()));
					return;
				}
				FCanonicalMemoryWriter CollisionWriter(
					CollisionBytes, EArchivePurpose::CookedPayload, {.Target = {"Win64", "Game"}});
				CollisionPayload.Serialize(CollisionWriter);
				auto CollisionBulk = CollisionWriter.IsError() ? std::expected<FBulkData, FBulkDataError>{}
				: FBulkData::TryCreateDetached(CollisionBytes);
				if (CollisionWriter.IsError() || !CollisionBulk)
				{
					Ar.Fail(EArchiveFailureCode::InvalidData, CollisionWriter.IsError()
						? CollisionWriter.GetFailure()->Message : FormatBulkDataError(CollisionBulk.error()));
					return;
				}
				CollisionProjection = std::move(*CollisionBulk);
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

	auto DStaticMesh::ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context) const -> std::expected<void, FObjectValidationError>
	{
		if (auto Result = Super::ValidateLoadedObjectGraph(Context); !Result) return Result;
		if (Context.bCooked && CookedRenderData.GetMetadata().LogicalSize == 0)
			return RejectLoadedObjectGraph(GetObjectPath(), "Required cooked RenderData field is missing.");
		if (MaterialSlots.size() > MaximumMeshMaterialSlots)
			return RejectLoadedObjectGraph(GetObjectPath(), "Material-slot count exceeds the supported limit.");
		std::unordered_set<FName> Names;
		for (const auto& Slot : MaterialSlots)
			if (Slot.Name.IsNone() || !Names.insert(Slot.Name).second)
				return RejectLoadedObjectGraph(GetObjectPath(), "Material-slot names must be non-None and unique.");
		const auto* Import = GetAssetImportData();
		if (!Context.bCooked && !GetSource().IsValid() && Import && Import->GetSourceData().FindByRole("source"))
			return RejectLoadedObjectGraph(GetObjectPath(), "Canonical imported geometry is missing or invalid.");
		return {};
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
		if (const auto Submitted = AsyncBuild({.Source = Source, .bMarkPackageDirty = false}); !Submitted)
		{
			DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), FormatStaticMeshBuildMessages(Submitted.error()));
			return;
		}
	}
	auto FormatCookedMeshLoadError(const FCookedMeshLoadError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case ECookedMeshLoadError::None: return {};
		case ECookedMeshLoadError::Admission:
			return Error.AdmissionCause ? FormatCookedMeshAdmissionError(*Error.AdmissionCause)
				: "Cooked mesh request admission failed.";
		case ECookedMeshLoadError::Cancelled: return "Cooked mesh loading was cancelled.";
		case ECookedMeshLoadError::Task: return "Cooked mesh decode task did not complete successfully.";
		case ECookedMeshLoadError::FieldCount: return std::format("Cooked mesh field count {} does not match {}.", Error.Actual, Error.Expected);
		case ECookedMeshLoadError::InvalidProduct: return "Cooked mesh publication product is invalid.";
		case ECookedMeshLoadError::CollisionPublication: return "Cooked mesh collision publication failed.";
		case ECookedMeshLoadError::CompletionBudget: return std::format("Cooked mesh result needs {} bytes with {} reserved against a {} byte mailbox limit.", Error.Actual, Error.Reserved, Error.Expected);
		case ECookedMeshLoadError::Unavailable: return "StaticMesh CPU render data is unavailable.";
		case ECookedMeshLoadError::Read:
		case ECookedMeshLoadError::RenderRead:
		case ECookedMeshLoadError::CollisionRead:
			return Error.ReadCause ? FormatPackageResourceReadError(*Error.ReadCause)
				: "Cooked mesh payload read failed.";
		case ECookedMeshLoadError::Product:
			return Error.ProductCause ? FormatCookedMeshProductError(*Error.ProductCause)
				: "Cooked mesh product decoding failed.";
		case ECookedMeshLoadError::Publication:
			return Error.PublicationCause ? (*Error.PublicationCause).ToString()
				: "Cooked mesh publication failed.";
		}
		return "Unknown cooked mesh load failure.";
	}

	auto DStaticMesh::LoadCookedRenderData() -> FCookedMeshLoadResult
	{
		const bool bRequiresCollision = BodySetup
			&& BodySetup->GetCollisionSourceMode() != EBodySetupCollisionSourceMode::None;
		auto Read = CookedRenderData.AcquireRead();
		if (!Read) return {.Error = {.Code = ECookedMeshLoadError::RenderRead, .Owner = FObjectKey(this),
			.ReadCause = std::make_shared<FPackageResourceReadResult>(std::move(Read.Error))}};
		const FByteView Bytes = Read.Lock.GetBytes();
		FBulkDataReadResult CollisionRead;
		FByteView CollisionBytes;
		if (bRequiresCollision)
		{
			CollisionRead = CookedCollisionData.AcquireRead();
			if (!CollisionRead) return {.Error = {.Code = ECookedMeshLoadError::CollisionRead, .Owner = FObjectKey(this),
				.ReadCause = std::make_shared<FPackageResourceReadResult>(std::move(CollisionRead.Error))}};
			CollisionBytes = CollisionRead.Lock.GetBytes();
		}

		FStaticMeshCookedProduct Product;
		const EBodySetupCollisionSourceMode CollisionMode = bRequiresCollision
			? BodySetup->GetCollisionSourceMode()
			: EBodySetupCollisionSourceMode::None;
		const EBodySetupCollisionQueryPolicy CollisionPolicy = BodySetup
			? BodySetup->GetCollisionQueryPolicy()
			: EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		if (const auto Result = DecodeStaticMeshCookedProduct(Bytes, CollisionBytes, MaterialSlots,
			CollisionMode, CollisionPolicy, Product); !Result)
		{
			return {.Error = {.Code = ECookedMeshLoadError::Product, .Owner = FObjectKey(this),
				.ProductCause = std::make_shared<FCookedMeshProductError>(Result.Error)}};
		}
		CollisionRead.Lock.Reset();
		Read.Lock.Reset();

		FStaticMeshRenderStateRecreateContext Refresh(this);
		if (const auto Published = CommitPreparedMeshData(
			std::move(Product.RenderData), nullptr); !Published)
		{
			return {.Error = {.Code = ECookedMeshLoadError::Publication, .Owner = FObjectKey(this),
				.PublicationCause = std::make_shared<FStaticMeshBuildFailure>(Published.error())}};
		}
		if (bRequiresCollision)
		{
			const bool bPublished = BodySetup->SetCollisionGeometry(
				Product.SimpleCollision, Product.ComplexCollision);
			check(bPublished);
		}
		return {};
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
				.Owner = FObjectKey(this),
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
				if (Cancellation.IsCancellationRequested()) return {.Error = {.Code = ECookedMeshLoadError::Cancelled}};
				if (Buffers.size() != (bRequiresCollision ? 2u : 1u))
					return {.Error = {.Code = ECookedMeshLoadError::FieldCount,
						.Actual = Buffers.size(), .Expected = bRequiresCollision ? 2u : 1u}};
				auto Result = std::make_unique<FStaticMeshManagerProduct>();
				const FByteView CollisionBytes = bRequiresCollision
					? Buffers[1].GetBytes() : FByteView{};
				if (const auto Decoded = DecodeStaticMeshCookedProduct(Buffers[0].GetBytes(),
					CollisionBytes, SlotSnapshot, CollisionMode, CollisionPolicy,
					Result->Product); !Decoded)
				{
					return {.Error = {.Code = ECookedMeshLoadError::Product,
						.ProductCause = std::make_shared<FCookedMeshProductError>(Decoded.Error)}};
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
				std::unique_ptr<ICookedMeshDetachedProduct> BaseProduct) -> FCookedMeshLoadResult {
				auto* Mesh = Cast<DStaticMesh>(&Owner);
				auto* Typed = dynamic_cast<FStaticMeshManagerProduct*>(
					BaseProduct.get());
				if (!Mesh || !Typed)
				{
					return {.Error = {.Code = ECookedMeshLoadError::InvalidProduct}};
				}
				FStaticMeshRenderStateRecreateContext RecreateContext(Mesh);
				FStaticMeshCookedProduct Product = std::move(Typed->Product);
				if (const auto Published = Mesh->CommitPreparedMeshData(
					std::move(Product.RenderData), nullptr); !Published)
				{
					return {.Error = {.Code = ECookedMeshLoadError::Publication,
						.PublicationCause = std::make_shared<FStaticMeshBuildFailure>(Published.error())}};
				}
				if (Product.bHasCollision)
				{
					if (!Mesh->BodySetup
						|| !Mesh->BodySetup->SetCollisionGeometry(
							Product.SimpleCollision, Product.ComplexCollision))
					{
						return {.Error = {.Code = ECookedMeshLoadError::CollisionPublication}};
					}
				}
				Mesh->CookedLoadPhase.store(
					ECookedMeshCpuPhase::CpuReady, std::memory_order_release);
				if (bInitializeResources) Mesh->InitResources();
				Mesh->CookedLoadError = {};
				return {};
			},
			.OnTerminal = [](DObject& Owner,
				const FCookedMeshLoadIdentity&,
				ECookedMeshTerminalState Terminal,
				const FCookedMeshLoadError& Error) {
				auto* Mesh = Cast<DStaticMesh>(&Owner);
				if (!Mesh) return;
				Mesh->CookedLoadError = Error;
				const bool bFailed = Terminal == ECookedMeshTerminalState::Failed
					|| Terminal == ECookedMeshTerminalState::Rejected;
				Mesh->CookedLoadPhase.store(bFailed
					? ECookedMeshCpuPhase::Failed
					: ECookedMeshCpuPhase::Cancelled, std::memory_order_release);
			}
		};
		if (bRequiresCollision)
			Request.Fields.push_back(CookedCollisionData);
		if (const auto Submitted = Manager->Submit(std::move(Request)); !Submitted)
		{
			CookedLoadError = {.Code = ECookedMeshLoadError::Admission, .Owner = FObjectKey(this),
				.AdmissionCause = std::make_shared<FCookedMeshAdmissionError>(Submitted.Error)};
			return false;
		}
		CookedLoadError = {};
		CookedLoadPhase.store(
			ECookedMeshCpuPhase::IoQueued, std::memory_order_release);
		return true;
	}

	auto DStaticMesh::ContributeToCook(FCookContext& Context,
		std::string_view VirtualPackagePath) -> FCookContributionResult
	{
		auto Reject = [&](ECookContributionError Error) -> FCookContributionResult {
			return {.Error = Error, .ObjectPath = GetObjectPath(), .VirtualPath = std::string(VirtualPackagePath),
				.TargetPlatform = Context.GetTargetPlatform(), .TargetProfile = Context.GetTargetProfile()};
		};
		if (Context.GetTargetPlatform() != ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != ECookTargetProfile::Game) return Reject(ECookContributionError::Target);
		if (!RenderData && !Source.IsValid()) return Reject(ECookContributionError::RenderData);
		const auto Added = Context.AddPackage(std::string(VirtualPackagePath), GetPackage());
		if (!Added)
		{
			auto Result = Reject(ECookContributionError::Plan);
			Result.PlanCause = Added.Error;
			return Result;
		}
		return {};
	}

}
