#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshCompilation.h"

#include "Asset/Asset.h"
#include "DObject/ObjectLifecycle.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace Durin
{
	auto FormatStaticMeshCacheDiagnostics(const FStaticMeshCacheDiagnostics& Diagnostic) -> std::string
	{
		auto Read = Diagnostic.DecodeCause ? FormatStaticMeshCacheCodecError(*Diagnostic.DecodeCause) : FormatAssetCacheDiagnostic(Diagnostic.Read);
		auto Write = FormatAssetCacheDiagnostic(Diagnostic.Write);
		constexpr size_t MaximumBytes = 2048;
		if (Read.empty()) return Write.substr(0, MaximumBytes);
		if (Write.empty()) return Read.substr(0, MaximumBytes);
		constexpr size_t Budget = (MaximumBytes - 13) / 2;
		return "Read: " + Read.substr(0, Budget) + "; Put: " + Write.substr(0, Budget);
	}

	auto FormatStaticMeshPersistenceDiagnostic(const FStaticMeshPersistenceDiagnostic& Diagnostic) -> std::string
	{
		auto Render = FormatStaticMeshCacheDiagnostics(Diagnostic.Render);
		auto Collision = FormatStaticMeshCacheDiagnostics(Diagnostic.Collision);
		if (Render.empty()) return Collision;
		if (Collision.empty()) return Render;
		return (Render + " " + Collision).substr(0, MaximumStaticMeshBuildDiagnosticBytes);
	}

	auto CaptureStaticMeshReconciliation(const DStaticMesh& Mesh)
		-> FStaticMeshReconciliationSnapshot
	{
		DBodySetup* Body = Mesh.GetBodySetup();
		return {.MaterialSlots = std::vector<FMeshMaterialSlotDefinition>(
				Mesh.GetMaterialSlots().begin(), Mesh.GetMaterialSlots().end()),
			.NormalizedSize = Mesh.GetNormalizedSize(),
			.SourceIdentity = Mesh.GetSource().GetIdentity(),
			.Body = FObjectKey(Body),
			.BodyRevision = Body ? Body->GetRevision() : 0,
			.CollisionMode = Body ? Body->GetCollisionSourceMode() : EBodySetupCollisionSourceMode::None,
			.CollisionPolicy = Body ? Body->GetCollisionQueryPolicy() : EBodySetupCollisionQueryPolicy::SimpleAndComplex};
	}

	auto MakeStaticMeshAuthoredBuildRequest(FStaticMeshSource Source,
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

	auto FormatStaticMeshAuthoredBuildError(const FStaticMeshAuthoredBuildError& Error) -> std::string
	{
		if (!Error.Message.empty()) return Error.Message.substr(0, MaximumStaticMeshBuildDiagnosticBytes);
		switch (Error.Code)
		{
		case EStaticMeshAuthoredBuildError::NotStarted: return "StaticMesh candidate build has not started.";
		case EStaticMeshAuthoredBuildError::Cancelled: return "StaticMesh candidate build was cancelled.";
		case EStaticMeshAuthoredBuildError::Input: return "StaticMesh candidate input/settings are invalid.";
		case EStaticMeshAuthoredBuildError::SourceBudget: return "StaticMesh predicted decoded source exceeds its reservation.";
		case EStaticMeshAuthoredBuildError::RenderBuild:
		case EStaticMeshAuthoredBuildError::CollisionBuild:
			return "StaticMesh derived-data build failed.";
		case EStaticMeshAuthoredBuildError::MaterialSlots: return "StaticMesh candidate has inconsistent material slots.";
		case EStaticMeshAuthoredBuildError::SlotName: return "StaticMesh candidate requires unique named material slots.";
		case EStaticMeshAuthoredBuildError::UVChannels: return "StaticMesh candidate exceeds the texture coordinate limit.";
		case EStaticMeshAuthoredBuildError::MetadataBudget: return "StaticMesh render metadata exceeds its reservation.";
		case EStaticMeshAuthoredBuildError::FinalizationBudget: return "StaticMesh predicted finalization working set exceeds its reservation.";
		case EStaticMeshAuthoredBuildError::Payload: return "StaticMesh payload validation failed.";
		case EStaticMeshAuthoredBuildError::LODPolicy: return "StaticMesh LOD policy validation failed.";
		case EStaticMeshAuthoredBuildError::ProviderChanged: return "StaticMesh provider changed while constructing the combined candidate.";
		case EStaticMeshAuthoredBuildError::RetainedBudget: return "StaticMesh retained candidate exceeds its reservation.";
		case EStaticMeshAuthoredBuildError::TaskRetired: return "StaticMesh task retired before worker completion.";
		case EStaticMeshAuthoredBuildError::WorkerException: return "StaticMesh worker failed with an exception.";
		}
		return {};
	}

	auto BuildStaticMeshAuthoredCandidate(FStaticMeshAuthoredBuildRequest Request,
		const FStaticMeshBuildExecutionControl& Control) -> std::expected<std::unique_ptr<FStaticMeshAuthoredCandidate>, FStaticMeshAuthoredBuildError>
	{
		const auto Fail = [](FStaticMeshAuthoredBuildError Error) {
			Error.Message = FormatStaticMeshAuthoredBuildError(Error);
			return std::unexpected(std::move(Error));
		};
		const auto BudgetFailure = [&](EStaticMeshAuthoredBuildError Code, const FStaticMeshBuildMemoryEstimate& Memory) {
			return Fail({.Code = Code, .Message = std::format("{} Limit {}, accumulated {}, rejected {} x {} bytes.",
				FormatStaticMeshAuthoredBuildError({.Code = Code}), Memory.Limit, Memory.Bytes, Memory.RejectedCount, Memory.RejectedWidth)});
		};
		if (Control.IsCancelled()) return Fail({.Code = EStaticMeshAuthoredBuildError::Cancelled});
		if (!Request.Source.IsValid() || !std::isfinite(Request.NormalizedSize) || Request.NormalizedSize <= 0
			|| Request.MaterialSlots.size() > MaximumMeshMaterialSlots
			|| (Request.CollisionMode != EBodySetupCollisionSourceMode::None
				&& Request.CollisionMode != EBodySetupCollisionSourceMode::ConvexHullFromLOD0
				&& Request.CollisionMode != EBodySetupCollisionSourceMode::TriangleMeshFromLOD0)
			|| (Request.CollisionPolicy != EBodySetupCollisionQueryPolicy::SimpleOnly
				&& Request.CollisionPolicy != EBodySetupCollisionQueryPolicy::ComplexOnly
				&& Request.CollisionPolicy != EBodySetupCollisionQueryPolicy::SimpleAndComplex))
			return Fail({.Code = EStaticMeshAuthoredBuildError::Input, .Message = std::format(
				"StaticMesh candidate input/settings are invalid (source {}, normalized size {}, slots {}/{}, collision mode {}, policy {}).",
				Request.Source.IsValid(), Request.NormalizedSize, Request.MaterialSlots.size(), MaximumMeshMaterialSlots,
				static_cast<uint8>(Request.CollisionMode), static_cast<uint8>(Request.CollisionPolicy))});
		FStaticMeshBuildMemoryEstimate SourceMemory{Control.MaximumWorkingSetBytes};
		if (!SourceMemory.Add(Request.Source.GetGeometryBulk().GetPayloadSize(), 8)
			|| !SourceMemory.Add(Request.Source.GetMeshCount(), sizeof(FStaticMeshImportedMesh))
			|| !SourceMemory.Add(Request.Source.GetMaterialSlotCount(), 32768))
			return BudgetFailure(EStaticMeshAuthoredBuildError::SourceBudget, SourceMemory);
		auto Candidate = std::unique_ptr<FStaticMeshAuthoredCandidate>(new FStaticMeshAuthoredCandidate);
		FStaticMeshReconciliationSnapshot Reconciliation;
		Reconciliation.NormalizedSize = Request.NormalizedSize;
		for (const auto& Slot : Request.MaterialSlots)
			Reconciliation.MaterialSlots.push_back({.Name = Slot.Name,
				.SourceName = Slot.SourceName, .SourceMaterialIndex = Slot.SourceMaterialIndex});
		const auto RenderOutcome = BuildStaticMeshDerivedData({.Reconciliation = std::move(Reconciliation),
			.Source = Request.Source, .bPersistDerivedData = Request.bPersistDerivedData},
			Candidate->Render, Control);
		if (!RenderOutcome)
		{
			return Fail({.Code = RenderOutcome.error().Code == EStaticMeshDerivedDataError::Cancelled
				? EStaticMeshAuthoredBuildError::Cancelled : EStaticMeshAuthoredBuildError::RenderBuild,
				.Message = FormatStaticMeshDerivedDataError(RenderOutcome.error())});
		}
		auto& Render = Candidate->Render;
		if (!Render.RenderData || Render.MaterialSlots.empty()
			|| Render.MaterialSlots.size() > MaximumMeshMaterialSlots
			|| Render.MaterialSlots.size() != Render.RenderData->MaterialSlots.size())
			return Fail({.Code = EStaticMeshAuthoredBuildError::MaterialSlots, .Message = std::format(
				"StaticMesh candidate has inconsistent material slots ({}, render {}).", Render.MaterialSlots.size(),
				Render.RenderData ? Render.RenderData->MaterialSlots.size() : 0)});
		std::unordered_set<FName> SlotNames;
		for (size_t Index = 0; Index < Render.MaterialSlots.size(); ++Index)
		{
			const auto& Slot = Render.MaterialSlots[Index];
			if (Slot.Name.IsNone() || !SlotNames.insert(Slot.Name).second)
				return Fail({.Code = EStaticMeshAuthoredBuildError::SlotName, .Message = std::format(
					"StaticMesh candidate requires unique named material slots (slot {}, name '{}').", Index, Slot.Name.ToString())});
		}
		for (size_t Index = 0; Index < Render.RenderData->LODResources.size(); ++Index)
		{
			const auto& LOD = Render.RenderData->LODResources[Index];
			if (LOD.NumTexCoords > MaxStaticMeshUVChannels)
				return Fail({.Code = EStaticMeshAuthoredBuildError::UVChannels, .Message = std::format(
					"StaticMesh candidate LOD {} has {} UV channels; maximum {}.", Index, LOD.NumTexCoords, MaxStaticMeshUVChannels)});
		}
		FStaticMeshBuildMemoryEstimate Memory{Control.MaximumWorkingSetBytes};
		if (!Memory.Add(1, 1024 * 1024) || !Memory.Add(Render.MaterialSlots.capacity(), 32768)
			|| !Memory.Add(Render.RenderData->LODResources.capacity(), sizeof(FStaticMeshLODResources)))
			return BudgetFailure(EStaticMeshAuthoredBuildError::MetadataBudget, Memory);
		for (const auto& LOD : Render.RenderData->LODResources)
		{
			if (!Memory.Add(LOD.VertexBuffers.PositionVertexBuffer.GetPositions().capacity(), 512)
				|| !Memory.Add(LOD.IndexBuffer.GetIndices().capacity(), 192)
				|| !Memory.Add(LOD.Sections.capacity(), sizeof(FStaticMeshSection)))
				return BudgetFailure(EStaticMeshAuthoredBuildError::FinalizationBudget, Memory);
		}
		bool bCancelled = false;
		const std::function<bool()> ShouldCancel = [&] {
			bCancelled = bCancelled || Control.IsCancelled();
			return bCancelled;
		};
		FStaticMeshPayloadData Payload;
		if (const auto Result = MakeStaticMeshPayloadData(*Render.RenderData, Payload, ShouldCancel); !Result)
			return Fail({.Code = Result.error().Code == EStaticMeshPayloadError::Cancelled ? EStaticMeshAuthoredBuildError::Cancelled : EStaticMeshAuthoredBuildError::Payload,
				.Message = FormatStaticMeshPayloadError(Result.error())});
		if (const auto Policy = ValidateStaticMeshLODScreenSizes(Render.RenderData->LODResources); !Policy)
			return Fail({.Code = bCancelled ? EStaticMeshAuthoredBuildError::Cancelled : EStaticMeshAuthoredBuildError::LODPolicy,
				.Message = FormatStaticMeshLODPolicyError(Policy.error())});
		if (Control.IsCancelled()) return Fail({.Code = EStaticMeshAuthoredBuildError::Cancelled});
		if (!Render.RenderData->RecalculateBounds(ShouldCancel))
			return Fail({.Code = EStaticMeshAuthoredBuildError::Cancelled, .Message = "StaticMesh bounds build was cancelled."});
#if DURIN_WITH_EDITOR
		for (auto& LOD : Render.RenderData->LODResources)
		{
			LOD.RayQueryAcceleration = BuildStaticMeshRayQueryAcceleration(LOD, ShouldCancel);
			if (bCancelled) return Fail({.Code = EStaticMeshAuthoredBuildError::Cancelled, .Message = "StaticMesh ray build was cancelled."});
			// An unavailable optional acceleration retains exact reference traversal.
		}
#endif
		FStaticMeshBuildExecutionControl CollisionControl = Control;
		CollisionControl.ExpectedProviderRegistration = Render.ProviderRegistration;
		const auto CollisionOutcome = BuildStaticMeshCollisionDerivedData(*Render.RenderData,
			Request.CollisionMode, Request.CollisionPolicy, Candidate->Collision,
			Request.bPersistDerivedData, CollisionControl);
		if (!CollisionOutcome)
		{
			return Fail({.Code = CollisionOutcome.error().Code == EStaticMeshDerivedDataError::Cancelled
				? EStaticMeshAuthoredBuildError::Cancelled : EStaticMeshAuthoredBuildError::CollisionBuild,
				.Message = FormatStaticMeshDerivedDataError(CollisionOutcome.error())});
		}
		if (Request.CollisionMode != EBodySetupCollisionSourceMode::None
			&& (Render.Descriptor.ProducerIdentity != Candidate->Collision.Descriptor.ProducerIdentity
				|| Render.Descriptor.RenderBuilderVersion != Candidate->Collision.Descriptor.RenderBuilderVersion
				|| Render.Descriptor.CollisionBuilderVersion != Candidate->Collision.Descriptor.CollisionBuilderVersion))
			return Fail({.Code = EStaticMeshAuthoredBuildError::ProviderChanged});
		if (Control.IsCancelled()) return Fail({.Code = EStaticMeshAuthoredBuildError::Cancelled});
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
		if (!bFits) return BudgetFailure(EStaticMeshAuthoredBuildError::RetainedBudget, Retained);
		Request.Source.ReleaseGeometry();
		Candidate->Request = std::move(Request);
		return Candidate;
	}

	auto FormatStaticMeshDirectBuildError(const FStaticMeshDirectBuildError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EStaticMeshDirectBuildError::None: return {};
		case EStaticMeshDirectBuildError::Render: return Error.RenderCause ? FormatStaticMeshReplacementError(*Error.RenderCause) : "StaticMesh render replacement failed.";
		case EStaticMeshDirectBuildError::Collision: return Error.CollisionCause ? FormatStaticMeshCollisionError(*Error.CollisionCause) : "StaticMesh collision replacement failed.";
		}
		return {};
	}

	auto ApplyStaticMeshBuildResult(DStaticMesh& Mesh,
		FStaticMeshSource Source, FStaticMeshBuildResult Product,
		bool bMarkPackageDirty) -> std::expected<void, FStaticMeshDirectBuildError>
	{
		const auto Replaced = Mesh.ReplaceSourceRenderData(std::move(Source),
			std::move(Product.RenderData), std::move(Product.MaterialSlots),
			Product.NormalizedSize);
		const bool bPublishedRenderData = static_cast<bool>(Replaced);
		if (bPublishedRenderData && Product.bSlotMetadataChanged)
		{
			ReportAssetLoadMutation(&Mesh, "Engine.StaticMesh.MaterialSlotsV1",
				"Static mesh material-slot identity metadata was upgraded.",
				EAssetLoadMutationKind::Upgrade);
		}
		// Source settings can change even if the subsequent build fails.
		if (bMarkPackageDirty || (bPublishedRenderData && Product.bSlotMetadataChanged)) Mesh.MarkPackageDirty();
		if (!Replaced) return std::unexpected(FStaticMeshDirectBuildError{.Code = EStaticMeshDirectBuildError::Render, .RenderCause = Replaced.error()});
		if (Mesh.GetCollisionBuildStatus() == EStaticMeshCollisionBuildStatus::Failed)
		{
			return std::unexpected(FStaticMeshDirectBuildError{.Code = EStaticMeshDirectBuildError::Collision, .CollisionCause = Mesh.GetCollisionBuildError()});
		}
		return {};
	}

	auto FormatStaticMeshSynchronousError(const FStaticMeshSynchronousError& Error) -> std::string
	{
		return Error.Message;
	}

	auto BuildStaticMeshSynchronously(DStaticMesh& Mesh,
		const FStaticMeshSource& Source) -> std::expected<FStaticMeshPersistenceDiagnostic, FStaticMeshSynchronousError>
	{
		if (!CanJoinStaticMeshCompilation(Mesh, Source))
		{
			if (const auto Submitted = SubmitStaticMeshCompilation(Mesh, {.Source = Source,
				.Priority = EStaticMeshCompilationPriority::Interactive}); !Submitted)
				return std::unexpected(FStaticMeshSynchronousError{.Code = EStaticMeshSynchronousError::Submission,
					.Message = FormatStaticMeshSubmissionError(Submitted.error()).substr(0, MaximumStaticMeshBuildDiagnosticBytes)});
		}
		FAssetCompilingManager::Get().FinishCompilationForObject(Mesh);
		auto Diagnostic = GetStaticMeshCompilationDiagnostic(Mesh);
		if (Diagnostic.RequestId == 0)
			return std::unexpected(FStaticMeshSynchronousError{.Code = EStaticMeshSynchronousError::NoObservation,
				.Message = "StaticMesh compilation completed without an available observation."});
		if (Diagnostic.Status != EStaticMeshCompilationStatus::Succeeded)
		{
			auto Code = EStaticMeshSynchronousError::Completion;
			auto Message = FormatStaticMeshCompilationDiagnostic(Diagnostic);
			if (Diagnostic.Status == EStaticMeshCompilationStatus::Cancelled)
			{
				Code = EStaticMeshSynchronousError::Cancelled;
				Message = "StaticMesh compilation was cancelled.";
			}
			else if (Diagnostic.Status == EStaticMeshCompilationStatus::Superseded)
			{
				Code = EStaticMeshSynchronousError::Superseded;
				Message = "StaticMesh compilation was superseded.";
			}
			return std::unexpected(FStaticMeshSynchronousError{.Code = Code, .Message = std::move(Message),
				.PersistenceDiagnostic = std::move(Diagnostic.PersistenceDiagnostic)});
		}
		return std::move(Diagnostic.PersistenceDiagnostic);
	}

	auto BuildStaticMeshSynchronously(DStaticMesh& Mesh,
		FStaticMeshDecodedGeometry Geometry) -> std::expected<FStaticMeshPersistenceDiagnostic, FStaticMeshSynchronousError>
	{
		FStaticMeshSource Source;
		if (const auto Initialized = Source.Initialize(std::move(Geometry)); !Initialized)
			return std::unexpected(FStaticMeshSynchronousError{.Code = EStaticMeshSynchronousError::Source,
				.Message = FormatStaticMeshSourceError(Initialized.error()).substr(0, MaximumStaticMeshBuildDiagnosticBytes)});
		return BuildStaticMeshSynchronously(Mesh, Source);
	}
}
