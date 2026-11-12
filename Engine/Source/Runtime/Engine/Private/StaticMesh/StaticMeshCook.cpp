#include "Physics/PhysicsDerivedData.h"
#include "Physics/PhysicsCookHelper.h"
#include "Physics/PhysicsMeshInputTask.h"
#include "Asset/OfflinePreparation.h"
#include "Asset/CookedPreparation.h"
#include "Logging/LogMacros.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMeshPayloadPrivate.h"
#include "Physics/PhysicsPayloadPrivate.h"
#include "Asset/CookedMeshProductsPrivate.h"
#include "StaticMesh/StaticMeshCustomVersion.h"

#include "DObject/Package.h"

#include "Asset/AssetCook.h"
#include "Asset/CookDependencies.h"
#include "Asset/CookedMeshProducts.h"
#include "StaticMesh/StaticMeshLoadUpdate.h"
#include "Threading/RunnableThread.h"
#include "DObject/Property.h"
#include "Hash/XxHash.h"
#include "Physics/BodySetup.h"
#include "Serialization/Archive.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshCompilation.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshRenderStateRecreateContext.h"
#if DURIN_WITH_EDITOR
#include "TargetPlatform.h"
#endif

namespace Durin
{
	namespace
	{
		auto BuildStaticCookedMetadataIdentity(const DStaticMesh& Mesh) -> uint64;
		auto CaptureMeshCookWork(DStaticMesh& Mesh, ECookTargetPlatform Platform, const FAssetBuildTaskContext& Control)
			-> std::expected<FCookDetachedWork, std::string>;
		auto EstimateMeshCookWork(DStaticMesh& Mesh) -> std::expected<uint64, std::string>
		{
			FAssetBuildMemoryEstimate Memory{MaximumAssetBuildRequestBytes, 1024 * 1024};
			if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) {
				if (!Memory.Add(Mesh.GetCookedRenderData().GetMetadata().LogicalSize, 1)
					|| !Memory.Add(Mesh.GetCookedCollisionData().GetMetadata().LogicalSize, 1))
					return std::unexpected("Cook mesh payload exceeds its working-set budget.");
				return Memory.Bytes;
			}
#if DURIN_WITH_EDITORONLY_DATA
			if (Mesh.GetSource().IsValid()) {
				if (!Memory.Add(Mesh.GetSource().GetGeometryBulk().GetPayloadSize(), 64)
					|| !Memory.Add(Mesh.GetSource().GetMeshCount(), 1024)
					|| !Memory.Add(Mesh.GetMaterialSlots().size() * Mesh.GetSource().GetNumLODs(), 32768))
					return std::unexpected("Cook mesh source exceeds its working-set budget.");
			} else
#endif
			if (const auto* Render = Mesh.GetRenderData()) {
				for (const auto& LOD : Render->LODResources)
					if (!Memory.Add(LOD.VertexBuffers.PositionVertexBuffer.GetPositions().size(), 512)
						|| !Memory.Add(LOD.IndexBuffer.GetIndices().size(), 192))
						return std::unexpected("Cook mesh projection exceeds its working-set budget.");
			}
			if (Mesh.GetBodySetup() && Mesh.GetBodySetup()->GetCollisionSourceMode() != EBodySetupCollisionSourceMode::None) {
				auto Input = Mesh.CreatePhysicsMeshInputTask();
				if (!Input) return std::unexpected(Input.error());
				if (!Memory.Add((*Input)->GetWorkingSetBytes(), 1)) return std::unexpected("Cook collision exceeds its working-set budget.");
			}
			return Memory.Bytes;
		}
	}

	auto DStaticMesh::BeginCacheForCookedPlatformData(FObjectCookContext& Context) -> void
	{
		Super::BeginCacheForCookedPlatformData(Context);
		RefreshComplexCollisionSource();
		auto* Preparation = FCookPreparationContext::Get(*this, Context);
		if (!Preparation) { Context.Result = {EObjectCookError::InvalidInput, "Missing mesh Cook preparation context."}; return; }
		if (Context.CheckCancellation()) return;
		if (bHasComplexCollisionSource && (ComplexCollisionPositions.empty() || ComplexCollisionIndices.empty()))
		{
			Context.Result = {EObjectCookError::InvalidInput, "Selected complex collision mesh has no usable input."};
			return;
		}
		if (HasPendingStaticMeshSourceMutation(*this)) {
			Preparation->Failure = {.Status = ECookContributionStatus::Error, .Error = ECookContributionError::SourceMutation,
				.ObjectPath = GetObjectPath(), .VirtualPath = Context.Package}; return;
		}
		auto Bytes = EstimateMeshCookWork(*this);
		if (!Bytes) { Context.Result = {EObjectCookError::Limit, std::move(Bytes.error())}; return; }
		if (!Preparation->ReserveWorkingSet(*Bytes)) return;
		const FAssetBuildTaskContext Control{.IsCancellationRequested = [&Context] { return Context.CheckCancellation(); }, .MaximumWorkingSetBytes = *Bytes};
		auto Captured = CaptureMeshCookWork(*this, Preparation->GetTargetPlatform(), Control);
		if (!Captured) { Context.Result = {EObjectCookError::Preparation, std::move(Captured.error())}; return; }
		const auto Metadata = BuildStaticCookedMetadataIdentity(*this);
		const auto RenderRevision = GetRenderResourceStatus().Revision;
		const auto CollisionRevision = GetCollisionSourceRevision();
#if DURIN_WITH_EDITORONLY_DATA
		const auto SourceIdentity = GetSource().GetIdentity();
		const auto Size = GetNormalizedSize();
#endif
		Preparation->ValidatePreparedInputs = [this, Metadata, RenderRevision, CollisionRevision
#if DURIN_WITH_EDITORONLY_DATA
			, SourceIdentity, Size
#endif
		] {
			return CollisionRevision == GetCollisionSourceRevision()
				&& Metadata == BuildStaticCookedMetadataIdentity(*this) && RenderRevision == GetRenderResourceStatus().Revision
#if DURIN_WITH_EDITORONLY_DATA
				&& SourceIdentity == GetSource().GetIdentity() && Size == GetNormalizedSize()
#endif
				;
		};
		Preparation->StartDetachedWork(std::move(*Captured));
	}

	auto DStaticMesh::OnCookEvent(ECookEvent Event, FObjectCookContext& Context) const -> void
	{
		Super::OnCookEvent(Event, Context);
		if (Event != ECookEvent::PlatformCookDependencies) return;
		if (const auto* Proxy = GetComplexCollisionMesh())
		{
			if (const auto* Package = Proxy->GetPackage(); Package && Package != GetPackage())
				Context.Dependencies.push_back({ECookBuildDependencyKind::DirectPackage, Package->GetPackagePath()});
		}
	}
	namespace
	{
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
			if (BodySetup)
			{
				Builder.UpdateValue(BodySetup->GetCollisionTraceMode());
				Builder.UpdateValue(BodySetup->GetSimpleCollision().size());
				for (const auto& Element : BodySetup->GetSimpleCollision())
				{
					Builder.UpdateValue(Element.Type);
					for (uint32 Axis = 0; Axis < 3; ++Axis)
					{
						Builder.UpdateValue(Element.Dimensions[Axis]);
						Builder.UpdateValue(Element.Transform.Translation[Axis]);
						Builder.UpdateValue(Element.Transform.Scale3D[Axis]);
					}
					for (uint32 Axis = 0; Axis < 4; ++Axis) Builder.UpdateValue(Element.Transform.Rotation[Axis]);
					Builder.UpdateValue(Element.Indices.size());
					for (uint32 Index : Element.Indices) Builder.UpdateValue(Index);
					Builder.UpdateValue(Element.Vertices.size());
					for (const auto& Vertex : Element.Vertices)
						for (uint32 Axis = 0; Axis < 3; ++Axis) Builder.UpdateValue(Vertex[Axis]);
				}
			}

			for (const FMeshMaterialSlotDefinition& Slot : Mesh.GetMaterialSlots())
			{
				Builder.Update(Slot.Name.ToString());
#if DURIN_WITH_EDITORONLY_DATA
				Builder.UpdateValue(Slot.SourceMaterialIndex);
#else
				Builder.UpdateValue(uint32{0});
#endif
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

	namespace
	{
		auto CaptureMeshCookWork(DStaticMesh& Mesh, ECookTargetPlatform Platform, const FAssetBuildTaskContext& Control)
			-> std::expected<FCookDetachedWork, std::string>
		{
			if (Control.IsCancelled()) return std::unexpected("Cook mesh capture was cancelled.");
			auto* Body = Mesh.GetBodySetup();
			const bool HasCollision = Body && Body->GetCollisionSourceMode() != EBodySetupCollisionSourceMode::None;
#if DURIN_WITH_EDITOR
			const auto Target = ResolveTargetPlatform(Platform);
			if (!Target || (HasCollision && !Target->SupportsCollisionPayload()))
#else
			if (GetTargetPlatformName(Platform).empty() || (HasCollision && Platform != ECookTargetPlatform::Win64))
#endif
				return std::unexpected("StaticMesh collision representation is unsupported for the requested target.");
			if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) {
				if (GetAssetRuntimeConfiguration().GetCookTargetPlatform() != Platform || !Mesh.GetCookedRenderData().HasData()
					|| (HasCollision && !Mesh.GetCookedCollisionData().HasData()))
					return std::unexpected("StaticMesh cooked data is unavailable or targets another platform.");
				return FCookDetachedWork([Product = FCookPreparationProducts{Mesh.GetCookedRenderData(), Mesh.GetCookedCollisionData()}]
					(const FAssetBuildTaskContext&) mutable -> std::expected<FCookPreparationProducts, std::string> { return std::move(Product); });
			}
#if DURIN_WITH_EDITORONLY_DATA
			std::optional<FStaticMeshBuildRequest> Request;
			if (Mesh.GetSource().IsValid()) {
				Request.emplace(FStaticMeshBuildRequest{.Settings = MakeStaticMeshBuildSettings(Mesh.GetMaterialSlots(), Mesh.GetNormalizedSize()), .Source = Mesh.GetSource()});
				Request->Source.ReleaseGeometry();
			}
#endif
			std::optional<FStaticMeshPayloadData> Projection;
#if DURIN_WITH_EDITORONLY_DATA
			if (!Request)
#endif
			{
				if (!Mesh.GetRenderData()) return std::unexpected("StaticMesh cooked render data is unavailable.");
				Projection.emplace();
				if (const auto Made = StaticMeshPrivate::TryMakeStaticMeshPayloadData(*Mesh.GetRenderData(), *Projection); !Made)
					return std::unexpected(StaticMeshPrivate::FormatStaticMeshPayloadError(Made.error()));
			}
			std::unique_ptr<FPhysicsMeshInputTask> Physics;
			FCookBodySetupInfo CookInfo;
			if (HasCollision) {
				auto Captured = Mesh.CreatePhysicsMeshInputTask();
				if (!Captured) return std::unexpected(Captured.error());
				Physics = std::move(*Captured); CookInfo = Body->GetCookInfo({});
				if (Physics->GetWorkingSetBytes() > Control.MaximumWorkingSetBytes)
					return std::unexpected("Cook physics input exceeds its reservation.");
			}
			return FCookDetachedWork([
#if DURIN_WITH_EDITORONLY_DATA
				Request = std::move(Request),
#endif
				Projection = std::move(Projection), Physics = std::move(Physics), CookInfo = std::move(CookInfo),
				Slots = std::vector<FMeshMaterialSlotDefinition>(Mesh.GetMaterialSlots().begin(), Mesh.GetMaterialSlots().end()), Platform](const FAssetBuildTaskContext& Control) mutable
				-> std::expected<FCookPreparationProducts, std::string> {
				if (Control.IsCancelled()) return std::unexpected("Cook mesh preparation was cancelled.");
#if DURIN_WITH_EDITORONLY_DATA
				if (Request) {
					auto Built = BuildStaticMeshRenderData(std::move(*Request), Control.MaximumWorkingSetBytes); Request.reset();
					if (Control.IsCancelled()) return std::unexpected("Cook render construction was cancelled.");
					if (!Built) return std::unexpected(Built.error());
					Projection.emplace();
					if (const auto Made = StaticMeshPrivate::TryMakeStaticMeshPayloadData(**Built, *Projection); !Made)
						return std::unexpected(StaticMeshPrivate::FormatStaticMeshPayloadError(Made.error()));
				}
#endif
				std::string Error;
				if (!ValidateStaticMeshMaterialSlotMapping(*Projection, Slots, Error)) return std::unexpected(std::move(Error));
				const FArchiveTarget ArchiveTarget{std::string(GetTargetPlatformName(Platform)), "Game"};
				FByteBuffer RenderBytes;
				FCanonicalMemoryWriter RenderWriter(RenderBytes, EArchivePurpose::CookedPayload, {.Target = ArchiveTarget});
				Projection->Serialize(RenderWriter); Projection.reset();
				if (RenderWriter.IsError()) return std::unexpected(RenderWriter.GetFailure()->Message);
				auto Render = FBulkData::TryCreateDetached(RenderBytes);
				if (!Render) return std::unexpected(FormatBulkDataError(Render.error()));
				FCookPreparationProducts Products{std::move(*Render), {}};
				if (Physics) {
					auto Data = Physics->Execute(Control); Physics.reset();
					if (Control.IsCancelled()) return std::unexpected("Cook collision preparation was cancelled.");
					if (!Data) return std::unexpected(Data.error());
					CookInfo.TriangleMeshDesc = std::move(*Data);
					auto Input = FPhysicsCookHelper::Capture(std::move(CookInfo), Control.MaximumWorkingSetBytes, static_cast<EAssetPayloadTargetPlatform>(Platform));
					if (!Input) return std::unexpected(Input.error());
					auto Built = FPhysicsCookHelper::CookCaptured(*Input, Control.MaximumWorkingSetBytes);
					if (!Built) return std::unexpected(Built.error());
					const auto& Geometry = Input->GetMode() == EBodySetupCollisionSourceMode::ConvexHullFromLOD0 ? Built->Simple : Built->Complex;
					FPhysicsCollisionPayloadData Payload;
					if (const auto Made = PhysicsPrivate::TryMakePhysicsCollisionPayloadData(Geometry, Payload); !Made)
						return std::unexpected(PhysicsPrivate::FormatPhysicsCollisionPayloadError(Made.error()));
					FByteBuffer Bytes;
					FCanonicalMemoryWriter Writer(Bytes, EArchivePurpose::CookedPayload, {.Target = ArchiveTarget});
					Payload.Serialize(Writer);
					if (Writer.IsError()) return std::unexpected(Writer.GetFailure()->Message);
					auto Bulk = FBulkData::TryCreateDetached(Bytes);
					if (!Bulk) return std::unexpected(FormatBulkDataError(Bulk.error()));
					Products.Collision = std::move(*Bulk);
				}
				if (Control.IsCancelled()) return std::unexpected("Cook mesh preparation was cancelled.");
				return Products;
			});
		}
	}

	auto DStaticMesh::PrepareCookedPayloads(ECookTargetPlatform Platform, FBulkData& Render,
		FBulkData& Collision, const FAssetBuildTaskContext& Control) -> std::expected<void, std::string>
	{
		Render = {}; Collision = {};
		auto Captured = CaptureMeshCookWork(*this, Platform, Control);
		if (!Captured) return std::unexpected(Captured.error());
		auto Built = (*Captured)(Control);
		if (!Built) return std::unexpected(Built.error());
		Render = std::move(Built->Primary); Collision = std::move(Built->Collision);
		return {};
	}

	auto DStaticMesh::SerializeCooked(FArchive& Ar) -> void
	{
		auto* Prepared = Ar.IsSaving() ? AssetPrivate::FScopedCookedPreparation::Find(*this, Ar.GetTarget()) : nullptr;
		if (Ar.IsSaving() && !Prepared)
		{
			Ar.Fail(EArchiveFailureCode::InvalidData, "StaticMesh serialization requires current prepared Cook data.");
			return;
		}
		Super::SerializeCooked(Ar);
		if (Ar.IsError()) return;
		if (!ParseTargetPlatform(Ar.GetTarget().Platform) || Ar.GetTarget().Profile != "Game")
		{
			Ar.Fail(EArchiveFailureCode::UnsupportedTarget, "StaticMesh cooked target is unsupported.");
			return;
		}
		FBulkData* RenderField = &CookedRenderData;
		FBulkData* CollisionField = &CookedCollisionData;
		if (Ar.IsSaving())
		{
			RenderField = &Prepared->Primary; CollisionField = &Prepared->Collision;
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
		if (GetComplexCollisionMesh() == this)
			return RejectLoadedObjectGraph(GetObjectPath(), "A mesh cannot be its own complex collision source.");
		if (ComplexCollisionPositions.size() > 6'000'000 || ComplexCollisionIndices.size() > 6'000'000
			|| (!bHasComplexCollisionSource && (!ComplexCollisionPositions.empty() || !ComplexCollisionIndices.empty())))
			return RejectLoadedObjectGraph(GetObjectPath(), "Complex collision input has invalid counts or source selection.");
		for (const auto& Position : ComplexCollisionPositions)
			if (!Math::IsFinite(Position))
				return RejectLoadedObjectGraph(GetObjectPath(), "Complex collision positions must be finite.");
		if (ComplexCollisionIndices.size() % 3 != 0)
			return RejectLoadedObjectGraph(GetObjectPath(), "Complex collision indices must form triangles.");
		for (uint32 Index : ComplexCollisionIndices)
			if (Index >= ComplexCollisionPositions.size())
				return RejectLoadedObjectGraph(GetObjectPath(), "Complex collision index is out of range.");
		if (Context.bCooked && CookedRenderData.GetMetadata().LogicalSize == 0)
			return RejectLoadedObjectGraph(GetObjectPath(), "Required cooked RenderData field is missing.");
		if (MaterialSlots.size() > MaximumMeshMaterialSlots)
			return RejectLoadedObjectGraph(GetObjectPath(), "Material-slot count exceeds the supported limit.");
		std::unordered_set<FName> Names;
		for (const auto& Slot : MaterialSlots)
			if (Slot.Name.IsNone() || !Names.insert(Slot.Name).second)
				return RejectLoadedObjectGraph(GetObjectPath(), "Material-slot names must be non-None and unique.");
#if DURIN_WITH_EDITORONLY_DATA
		const auto* Import = GetAssetImportData();
		if (!Context.bCooked && !GetSource().IsValid() && Import && Import->GetSourceData().FindByRole("source"))
			return RejectLoadedObjectGraph(GetObjectPath(), "Canonical imported geometry is missing or invalid.");
#else
		if (!Context.bCooked) return RejectLoadedObjectGraph(GetObjectPath(),
			"Authored StaticMesh packages require editor-only data support.");
#endif

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
#if DURIN_WITH_EDITORONLY_DATA
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
		if (CanJoinStaticMeshCompilation(*this, GetSource())) return;
		if (const auto Submitted = Build(EStaticMeshBuildMode::Asynchronous, {.bMarkPackageDirty = false}); !Submitted)
		{
			DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Submitted.error());
			return;
		}
#endif

	}
	auto FormatCookedMeshLoadError(const FCookedMeshLoadError& Error) -> std::string
	{
		if (!Error.Diagnostic.empty()) return Error.Diagnostic;
		switch (Error.Code)
		{
		case ECookedMeshLoadError::None: return {};
		case ECookedMeshLoadError::Unavailable: return "StaticMesh CPU render data is unavailable.";
		case ECookedMeshLoadError::Read: return "Cooked mesh payload read failed.";
		case ECookedMeshLoadError::Product: return "Cooked mesh product decoding failed.";
		case ECookedMeshLoadError::Publication: return "Cooked mesh publication failed.";
		case ECookedMeshLoadError::Cancelled: return "Cooked mesh loading was cancelled.";
		case ECookedMeshLoadError::Task: return "Cooked mesh decode task did not complete successfully.";
		case ECookedMeshLoadError::CompletionBudget: return "Cooked mesh result exceeds the completion byte budget.";
		}
		return "Unknown cooked mesh load failure.";
	}

	FStaticMeshLoadUpdate::FStaticMeshLoadUpdate(DStaticMesh& InMesh, bool bInInitializeResources)
		: Mesh(&InMesh), ResourceRevision(InMesh.GetRenderResourceStatus().Revision),
		  MetadataIdentity(BuildStaticCookedMetadataIdentity(InMesh)),
		  bInitializeResources(bInInitializeResources), RenderBulk(InMesh.CookedRenderData),
		  CollisionBulk(InMesh.CookedCollisionData), MaterialSlots(InMesh.MaterialSlots),
		  CollisionMode(InMesh.BodySetup ? InMesh.BodySetup->GetCollisionSourceMode() : EBodySetupCollisionSourceMode::None),
		  TargetPlatform(GetAssetRuntimeConfiguration().GetCookTargetPlatform())
	{
		const uint64 RenderBytes = RenderBulk.GetMetadata().LogicalSize;
		const uint64 CollisionBytes = CollisionMode != EBodySetupCollisionSourceMode::None
			? CollisionBulk.GetMetadata().LogicalSize : 0;
		if (RenderBytes == 0 || RenderBytes > MaximumBulkDataBytes
			|| (CollisionMode != EBodySetupCollisionSourceMode::None && CollisionBytes == 0)
			|| CollisionBytes > MaximumBulkDataBytes || RenderBytes > MaximumBulkDataBytes - CollisionBytes)
			Error = {.Code = ECookedMeshLoadError::Read,
				.Diagnostic = std::format("Cooked mesh render/collision field sizes are invalid: {} / {} bytes.", RenderBytes, CollisionBytes)};
		else EstimatedBytes = RenderBytes + CollisionBytes;
	}

	FStaticMeshLoadUpdate::~FStaticMeshLoadUpdate()
	{
		check(IsCompleted());
		check(!Task.IsValid() || Task.IsCompleted());
	}

	auto FStaticMeshLoadUpdate::Start(FTaskAttribution InAttribution) -> void
	{
		CheckGameThread();
		check(Phase == EPhase::Pending && Error.Code == ECookedMeshLoadError::None);
		Attribution = InAttribution;
		Reads.push_back(RenderBulk.ReloadAsync());
		if (CollisionMode != EBodySetupCollisionSourceMode::None)
			Reads.push_back(CollisionBulk.ReloadAsync());
		Phase = EPhase::Reading;
	}

	auto FStaticMeshLoadUpdate::Abort() -> void
	{
		CheckGameThread();
		if (IsCompleted()) return;
		Cancellation.Cancel();
		for (auto& Read : Reads) Read.Cancel();
	}

	auto FStaticMeshLoadUpdate::Decode(FByteView RenderBytes, FByteView CollisionBytes) -> void
	{
		if (IsCancelled()) return;
		if (const auto Decoded = AssetPrivate::TryDecodeStaticMeshCookedProduct(
			RenderBytes, CollisionBytes, MaterialSlots, CollisionMode, Product, TargetPlatform); !Decoded)
		{
			Error = {.Code = ECookedMeshLoadError::Product,
				.Diagnostic = AssetPrivate::FormatCookedMeshProductError(Decoded.error())};
			return;
		}
		RetainedBytes = EstimatedBytes;
		if (Product.SimpleCollision) RetainedBytes += Product.SimpleCollision.GetRetainedBytes();
		if (Product.ComplexCollision) RetainedBytes += Product.ComplexCollision.GetRetainedBytes();
	}

	auto FStaticMeshLoadUpdate::Complete() -> void
	{
		CheckGameThread();
		check(!Task.IsValid() || Task.IsCompleted());
		if (IsCancelled() || Mesh->IsPendingKill() || Mesh->PendingUpdate.get() != this
			|| Mesh->GetRenderResourceStatus().Revision != ResourceRevision
			|| BuildStaticCookedMetadataIdentity(*Mesh) != MetadataIdentity)
		{
			Cancellation.Cancel();
			Error = {.Code = ECookedMeshLoadError::Cancelled};
		}
		if (Error.Code == ECookedMeshLoadError::None)
		{
			FStaticMeshRenderStateRecreateContext Refresh(Mesh);
			if (const auto Published = Mesh->CommitPreparedMeshData(std::move(Product.RenderData), nullptr); !Published)
				Error = {.Code = ECookedMeshLoadError::Publication, .Diagnostic = Published.error()};
			else if (Product.bHasCollision && (!Mesh->BodySetup
				|| !Mesh->BodySetup->InstallPhysicsMeshes(Product.SimpleCollision, Product.ComplexCollision)))
				Error = {.Code = ECookedMeshLoadError::Publication,
					.Diagnostic = "Cooked mesh collision publication failed."};
			if (Error.Code == ECookedMeshLoadError::None && bInitializeResources) Mesh->InitResources();
		}
		Phase = EPhase::Completed;
		if (Mesh->PendingUpdate.get() == this)
		{
			Mesh->CookedLoadError = Error;
			Mesh->PendingUpdate.reset();
		}
		Mesh = nullptr;
		Reads.clear();
		Product = {};
		RenderBulk = {};
		CollisionBulk = {};
		MaterialSlots.clear();
		RetainedBytes = 0;
	}

	auto FStaticMeshLoadUpdate::Tick() -> void
	{
		CheckGameThread();
		if (IsCompleted()) return;
		if (Phase == EPhase::Pending)
		{
			if (IsCancelled() || Error.Code != ECookedMeshLoadError::None) Complete();
			return;
		}
		if (Phase == EPhase::Reading)
		{
			if (!std::ranges::all_of(Reads, [](const auto& Read) { return Read.IsReady(); })) return;
			std::vector<FSharedByteBuffer> Buffers;
			for (const auto& Read : Reads)
			{
				auto Result = Read.Wait();
				if (!Result)
				{
					if (Result.error().Status == EPackageResourceReadStatus::Cancelled) Cancellation.Cancel();
					Error = {.Code = ECookedMeshLoadError::Read,
						.Diagnostic = FormatPackageResourceReadError(Result.error())};
					break;
				}
				Buffers.push_back(std::move(*Result));
			}
			Reads.clear();
			if (IsCancelled() || Error.Code != ECookedMeshLoadError::None) { Complete(); return; }
			Phase = EPhase::Working;
			Task = Tasks::FTask(Tasks::LaunchTask("CookedMeshDecode",
				[this, Buffers = std::move(Buffers)] {
					Decode(Buffers[0].GetBytes(), Buffers.size() > 1 ? Buffers[1].GetBytes() : FByteView{});
				}, {.Attribution = Attribution}));
			return;
		}
		if (Phase == EPhase::Ready || (Phase == EPhase::Working && Task.IsCompleted()))
		{
			if (Task.GetState() == ETaskState::Canceled) Cancellation.Cancel();
			else if (Task.GetState() != ETaskState::Succeeded)
				Error = {.Code = ECookedMeshLoadError::Task, .Diagnostic = Task.GetDiagnostic()};
			Complete();
		}
	}

	auto FStaticMeshLoadUpdate::Finish() -> void
	{
		CheckGameThread();
		if (IsCompleted()) return;
		if (Phase == EPhase::Pending)
		{
			if (!IsCancelled() && Error.Code == ECookedMeshLoadError::None)
			{
				// Blocking callers use the same detached decoder and publication step,
				// including before the asynchronous scheduler has been initialized.
				auto RenderRead = RenderBulk.AcquireRead();
				if (!RenderRead)
					Error = {.Code = ECookedMeshLoadError::Read, .Diagnostic = RenderRead.error().Message};
				else
				{
					std::optional<FBulkDataReadScope> CollisionRead;
					if (CollisionMode != EBodySetupCollisionSourceMode::None)
					{
						auto Read = CollisionBulk.AcquireRead();
						if (!Read) Error = {.Code = ECookedMeshLoadError::Read, .Diagnostic = Read.error().Message};
						else CollisionRead.emplace(std::move(*Read));
					}
					if (Error.Code == ECookedMeshLoadError::None)
						Decode(RenderRead->GetBytes(), CollisionRead ? CollisionRead->GetBytes() : FByteView{});
				}
			}
			Complete();
			return;
		}
		while (!IsCompleted())
		{
			if (Phase == EPhase::Reading)
				for (const auto& Read : Reads) (void)Read.Wait();
			if (Phase == EPhase::Working) (void)Task.Wait();
			Tick();
		}
	}
}
