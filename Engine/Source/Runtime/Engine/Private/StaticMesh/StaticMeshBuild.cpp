#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshCompilation.h"

#include "Asset/Asset.h"
#include "DObject/ObjectLifecycle.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace Durin
{
	auto CaptureStaticMeshReconciliation(const DStaticMesh& Mesh)
		-> FStaticMeshReconciliationSnapshot
	{
		DBodySetup* Body = Mesh.GetBodySetup();
		return {.MaterialSlots = std::vector<FMeshMaterialSlotDefinition>(
				Mesh.GetMaterialSlots().begin(), Mesh.GetMaterialSlots().end()),
			.NormalizedSize = Mesh.GetNormalizedSize(),
			.SourceIdentity = Mesh.GetImportedData().GetIdentity(),
			.Body = MakeObjectHandle(Body),
			.BodyRevision = Body ? Body->GetRevision() : 0,
			.CollisionMode = Body ? Body->GetCollisionSourceMode() : EBodySetupCollisionSourceMode::None,
			.CollisionPolicy = Body ? Body->GetCollisionQueryPolicy() : EBodySetupCollisionQueryPolicy::SimpleAndComplex};
	}

	auto MakeStaticMeshAuthoredBuildRequest(FStaticMeshImportedData Source,
		const FStaticMeshReconciliationSnapshot& Snapshot) -> FStaticMeshAuthoredBuildRequest
	{
		FStaticMeshAuthoredBuildRequest Request;
		Request.Source = std::move(Source);
		Request.NormalizedSize = Snapshot.NormalizedSize;
		Request.CollisionMode = Snapshot.CollisionMode;
		Request.CollisionPolicy = Snapshot.CollisionPolicy;
		for (const auto& Slot : Snapshot.MaterialSlots)
			Request.MaterialSlots.push_back({Slot.Name, Slot.SourceName, Slot.SourceMaterialIndex});
		return Request;
	}

	auto BuildStaticMeshAuthoredCandidate(FStaticMeshAuthoredBuildRequest Request,
		std::unique_ptr<FStaticMeshAuthoredCandidate>& OutCandidate, std::string& OutError,
		const FStaticMeshBuildExecutionControl& Control) -> FStaticMeshBuildOutcome
	{
		OutCandidate.reset();
		OutError.clear();
		const auto Fail = [&](std::string Message, EStaticMeshBuildStatus Status = EStaticMeshBuildStatus::Failed) {
			FStaticMeshBuildOutcome Outcome(Status, Message);
			OutError = Outcome.Diagnostic;
			return Outcome;
		};
		if (Control.IsCancelled()) return Fail("StaticMesh candidate build was cancelled.", EStaticMeshBuildStatus::Cancelled);
		if (!Request.Source.IsValid() || !std::isfinite(Request.NormalizedSize) || Request.NormalizedSize <= 0
			|| Request.MaterialSlots.size() > MaximumMeshMaterialSlots
			|| (Request.CollisionMode != EBodySetupCollisionSourceMode::None
				&& Request.CollisionMode != EBodySetupCollisionSourceMode::ConvexHullFromLOD0
				&& Request.CollisionMode != EBodySetupCollisionSourceMode::TriangleMeshFromLOD0)
			|| (Request.CollisionPolicy != EBodySetupCollisionQueryPolicy::SimpleOnly
				&& Request.CollisionPolicy != EBodySetupCollisionQueryPolicy::ComplexOnly
				&& Request.CollisionPolicy != EBodySetupCollisionQueryPolicy::SimpleAndComplex))
			return Fail("StaticMesh candidate input/settings are invalid.");
		FStaticMeshBuildMemoryEstimate SourceMemory{Control.MaximumWorkingSetBytes};
		if (!SourceMemory.Add(Request.Source.GetGeometryBulk().GetPayloadSize(), 8)
			|| !SourceMemory.Add(Request.Source.GetMeshCount(), sizeof(FStaticMeshImportedMesh))
			|| !SourceMemory.Add(Request.Source.GetMaterialSlotCount(), 32768))
			return Fail("StaticMesh predicted decoded source exceeds its reservation.");
		auto Candidate = std::unique_ptr<FStaticMeshAuthoredCandidate>(new FStaticMeshAuthoredCandidate);
		FStaticMeshReconciliationSnapshot Reconciliation;
		Reconciliation.NormalizedSize = Request.NormalizedSize;
		for (const auto& Slot : Request.MaterialSlots)
			Reconciliation.MaterialSlots.push_back({.Name = Slot.Name,
				.SourceName = Slot.SourceName, .SourceMaterialIndex = Slot.SourceMaterialIndex});
		const auto RenderOutcome = BuildStaticMeshDerivedData({.Reconciliation = std::move(Reconciliation),
			.ImportedData = Request.Source, .bPersistDerivedData = Request.bPersistDerivedData},
			Candidate->Render, OutError, Control);
		if (!RenderOutcome) return RenderOutcome;
		auto& Render = Candidate->Render;
		if (!Render.RenderData || Render.MaterialSlots.empty()
			|| Render.MaterialSlots.size() > MaximumMeshMaterialSlots
			|| Render.MaterialSlots.size() != Render.RenderData->MaterialSlots.size())
			return Fail("StaticMesh candidate has inconsistent material slots.");
		std::unordered_set<FName> SlotNames;
		for (const auto& Slot : Render.MaterialSlots)
			if (Slot.Name.IsNone() || !SlotNames.insert(Slot.Name).second)
				return Fail("StaticMesh candidate requires unique named material slots.");
		for (const auto& LOD : Render.RenderData->LODResources)
			if (LOD.NumTexCoords > MaxStaticMeshUVChannels)
				return Fail("StaticMesh candidate exceeds the texture coordinate limit.");
		FStaticMeshBuildMemoryEstimate Memory{Control.MaximumWorkingSetBytes};
		if (!Memory.Add(1, 1024 * 1024) || !Memory.Add(Render.MaterialSlots.capacity(), 32768)
			|| !Memory.Add(Render.RenderData->LODResources.capacity(), sizeof(FStaticMeshLODResources)))
			return Fail("StaticMesh render metadata exceeds its reservation.");
		for (const auto& LOD : Render.RenderData->LODResources)
		{
			if (!Memory.Add(LOD.VertexBuffers.PositionVertexBuffer.GetPositions().capacity(), 512)
				|| !Memory.Add(LOD.IndexBuffer.GetIndices().capacity(), 192)
				|| !Memory.Add(LOD.Sections.capacity(), sizeof(FStaticMeshSection)))
				return Fail("StaticMesh predicted finalization working set exceeds its reservation.");
		}
		bool bCancelled = false;
		const std::function<bool()> ShouldCancel = [&] {
			bCancelled = bCancelled || Control.IsCancelled();
			return bCancelled;
		};
		FStaticMeshPayloadData Payload;
		if (!MakeStaticMeshPayloadData(*Render.RenderData, Payload, OutError, ShouldCancel)
			|| !ValidateStaticMeshLODScreenSizes(Render.RenderData->LODResources, OutError))
			return Fail(OutError, bCancelled ? EStaticMeshBuildStatus::Cancelled : EStaticMeshBuildStatus::Failed);
		if (Control.IsCancelled()) return Fail("StaticMesh candidate build was cancelled.", EStaticMeshBuildStatus::Cancelled);
		if (!Render.RenderData->RecalculateBounds(ShouldCancel))
			return Fail("StaticMesh bounds construction was cancelled.", EStaticMeshBuildStatus::Cancelled);
#if DURIN_WITH_EDITOR
		for (auto& LOD : Render.RenderData->LODResources)
		{
			LOD.RayQueryAcceleration = BuildStaticMeshRayQueryAcceleration(LOD, ShouldCancel);
			if (bCancelled) return Fail("StaticMesh ray construction was cancelled.", EStaticMeshBuildStatus::Cancelled);
			// An unavailable optional acceleration retains exact reference traversal.
		}
#endif
		FStaticMeshBuildExecutionControl CollisionControl = Control;
		CollisionControl.ExpectedProviderRegistration = Render.ProviderRegistration;
		const auto CollisionOutcome = BuildStaticMeshCollisionDerivedData(*Render.RenderData,
			Request.CollisionMode, Request.CollisionPolicy, Candidate->Collision,
			OutError, Request.bPersistDerivedData, CollisionControl);
		if (!CollisionOutcome) return CollisionOutcome;
		if (Request.CollisionMode != EBodySetupCollisionSourceMode::None
			&& (Render.Descriptor.ProducerIdentity != Candidate->Collision.Descriptor.ProducerIdentity
				|| Render.Descriptor.RenderBuilderVersion != Candidate->Collision.Descriptor.RenderBuilderVersion
				|| Render.Descriptor.CollisionBuilderVersion != Candidate->Collision.Descriptor.CollisionBuilderVersion))
			return Fail("StaticMesh provider changed while constructing the combined candidate.");
		if (Control.IsCancelled()) return Fail("StaticMesh candidate build was cancelled.", EStaticMeshBuildStatus::Cancelled);
		FStaticMeshBuildMemoryEstimate Retained{Control.MaximumWorkingSetBytes};
		bool bFits = Retained.Add(Request.Source.GetGeometryBulk().GetPayloadSize(), 1)
			&& Retained.Add(Render.MaterialSlots.capacity(), 32768)
			&& Retained.Add(Render.RenderData->LODResources.capacity(), sizeof(FStaticMeshLODResources))
			&& Retained.Add(Candidate->Collision.Simple.GetRetainedBytes(), 1)
			&& Retained.Add(Candidate->Collision.Complex.GetRetainedBytes(), 1);
		for (const auto& LOD : Render.RenderData->LODResources)
		{
			const auto& Buffers = LOD.VertexBuffers;
			bFits = bFits && Retained.Add(Buffers.PositionVertexBuffer.GetPositions().capacity(), sizeof(FVector3f))
				&& Retained.Add(Buffers.StaticMeshVertexBuffer.TangentsVertexBuffer.GetNormals().capacity(), sizeof(FVector3f))
				&& Retained.Add(Buffers.StaticMeshVertexBuffer.TangentsVertexBuffer.GetTangents().capacity(), sizeof(FVector4f))
				&& Retained.Add(Buffers.ColorVertexBuffer.GetColors().capacity(), sizeof(FVector4f))
				&& Retained.Add(LOD.IndexBuffer.GetIndices().capacity(), sizeof(uint32))
				&& Retained.Add(LOD.Sections.capacity(), sizeof(FStaticMeshSection));
			for (const auto& UV : Buffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.GetTexCoords())
				bFits = bFits && Retained.Add(UV.capacity(), sizeof(FVector2f));
			if (LOD.RayQueryAcceleration)
				bFits = bFits && Retained.Add(LOD.RayQueryAcceleration->RetainedBytes, 1);
		}
		if (!bFits) return Fail("StaticMesh retained candidate exceeds its reservation.");
		Request.Source.ReleaseGeometry();
		Candidate->Request = std::move(Request);
		OutCandidate = std::move(Candidate);
		return {EStaticMeshBuildStatus::Succeeded};
	}

	auto ApplyStaticMeshBuildResult(DStaticMesh& Mesh,
		FStaticMeshImportedData Source, FStaticMeshBuildResult Product, std::string& OutError,
		bool bMarkPackageDirty) -> bool
	{
		if (!Mesh.SetImportedRenderData(std::move(Source),
			std::move(Product.RenderData), std::move(Product.MaterialSlots),
			Product.NormalizedSize, OutError)) return false;
		if (Product.bSlotMetadataChanged)
		{
			ReportAssetLoadMutation(&Mesh, "Engine.StaticMesh.MaterialSlotsV1",
				"Static mesh material-slot identity metadata was upgraded.",
				EAssetLoadMutationKind::Upgrade);
		}
		if (bMarkPackageDirty || Product.bSlotMetadataChanged) Mesh.MarkPackageDirty();
		return true;
	}

	auto BuildStaticMeshSynchronously(DStaticMesh& Mesh,
		const FStaticMeshImportedData& ImportedData,
		std::string& OutError) -> bool
	{
		if (!CanJoinStaticMeshCompilation(Mesh, ImportedData)
			&& !SubmitStaticMeshCompilation(Mesh, {.Source = ImportedData,
				.Priority = EStaticMeshCompilationPriority::Interactive}, OutError)) return false;
		FAssetCompilingManager::Get().FinishCompilationForObject(Mesh);
		const auto Diagnostic = GetStaticMeshCompilationDiagnostic(Mesh);
		OutError = Diagnostic.Message;
		return Diagnostic.Status == EStaticMeshCompilationStatus::Succeeded;
	}

	auto BuildStaticMeshSynchronously(DStaticMesh& Mesh,
		FStaticMeshDecodedGeometry Geometry, std::string& OutError) -> bool
	{
		FStaticMeshImportedData Source;
		return Source.Initialize(std::move(Geometry), OutError)
			&& BuildStaticMeshSynchronously(Mesh, Source, OutError);
	}
}
