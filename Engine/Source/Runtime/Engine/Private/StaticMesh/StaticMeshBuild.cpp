#include "StaticMesh/StaticMeshBuilder.h"
#include "StaticMesh/StaticMeshCompilation.h"

#include "Asset/Asset.h"
#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"
#include "Logging/LogMacros.h"
#include "DObject/ObjectLifecycle.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace Durin
{
	auto FStaticMeshCacheError::ToString() const -> std::string
	{
		const auto OperationName = Operation == EStaticMeshCacheOperation::Read ? "read"
			: Operation == EStaticMeshCacheOperation::Decode ? "decode" : "write";
		return std::format("StaticMesh {} cache {}: {}",
			Kind == EStaticMeshRecipeKind::Render ? "render" : "collision", OperationName, Message);
	}

	auto FStaticMeshBuilder::Capture(const DStaticMesh& Mesh)
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

	auto FStaticMeshBuilder::MakeRequest(FStaticMeshSource Source,
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

	auto FStaticMeshBuilder::BuildCandidate(FStaticMeshAuthoredBuildRequest Request,
		const FStaticMeshBuildExecutionControl& Control) -> std::expected<std::unique_ptr<FStaticMeshAuthoredCandidate>, FStaticMeshBuildFailure>
	{
		const auto Fail = [](FStaticMeshBuildFailure Error) {
			return std::unexpected(std::move(Error));
		};
		const auto BudgetFailure = [&](std::string_view Reason, const FStaticMeshBuildMemoryEstimate& Memory) {
			return Fail(FStaticMeshBuildFailure{std::format("{} Limit {}, accumulated {}, rejected {} x {} bytes.",
				Reason, Memory.Limit, Memory.Bytes, Memory.RejectedCount, Memory.RejectedWidth), EStaticMeshBuildStage::Validation});
		};
		if (Control.IsCancelled()) return Fail(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Validation, "StaticMesh candidate build was cancelled."));
		if (!Request.Source.IsValid() || !std::isfinite(Request.NormalizedSize) || Request.NormalizedSize <= 0
			|| Request.MaterialSlots.size() > MaximumMeshMaterialSlots
			|| (Request.CollisionMode != EBodySetupCollisionSourceMode::None
				&& Request.CollisionMode != EBodySetupCollisionSourceMode::ConvexHullFromLOD0
				&& Request.CollisionMode != EBodySetupCollisionSourceMode::TriangleMeshFromLOD0)
			|| (Request.CollisionPolicy != EBodySetupCollisionQueryPolicy::SimpleOnly
				&& Request.CollisionPolicy != EBodySetupCollisionQueryPolicy::ComplexOnly
				&& Request.CollisionPolicy != EBodySetupCollisionQueryPolicy::SimpleAndComplex))
			return Fail(FStaticMeshBuildFailure{std::format(
				"StaticMesh candidate input/settings are invalid (source {}, normalized size {}, slots {}/{}, collision mode {}, policy {}).",
				Request.Source.IsValid(), Request.NormalizedSize, Request.MaterialSlots.size(), MaximumMeshMaterialSlots,
				static_cast<uint8>(Request.CollisionMode), static_cast<uint8>(Request.CollisionPolicy)), EStaticMeshBuildStage::Source});
		FStaticMeshBuildMemoryEstimate SourceMemory{Control.MaximumWorkingSetBytes};
		if (!SourceMemory.Add(Request.Source.GetGeometryBulk().GetPayloadSize(), 8)
			|| !SourceMemory.Add(Request.Source.GetMeshCount(), sizeof(FStaticMeshImportedMesh))
			|| !SourceMemory.Add(Request.Source.GetMaterialSlotCount(), 32768))
			return BudgetFailure("StaticMesh predicted decoded source exceeds its reservation.", SourceMemory);
		auto Candidate = std::unique_ptr<FStaticMeshAuthoredCandidate>(new FStaticMeshAuthoredCandidate);
		FStaticMeshReconciliationSnapshot Reconciliation;
		Reconciliation.NormalizedSize = Request.NormalizedSize;
		for (const auto& Slot : Request.MaterialSlots)
			Reconciliation.MaterialSlots.push_back({.Name = Slot.Name,
				.SourceName = Slot.SourceName, .SourceMaterialIndex = Slot.SourceMaterialIndex});
		auto RenderOutcome = FStaticMeshBuilder::Build({.Reconciliation = std::move(Reconciliation),
			.Source = Request.Source, .bPersistDerivedData = Request.bPersistDerivedData},
			Control);
		if (!RenderOutcome)
		{
			return Fail(RenderOutcome.error());
		}
		Candidate->Render = std::move(*RenderOutcome);
		auto& Render = Candidate->Render;
		if (!Render.RenderData || Render.MaterialSlots.empty()
			|| Render.MaterialSlots.size() > MaximumMeshMaterialSlots
			|| Render.MaterialSlots.size() != Render.RenderData->MaterialSlots.size())
			return Fail(FStaticMeshBuildFailure{std::format(
				"StaticMesh candidate has inconsistent material slots ({}, render {}).", Render.MaterialSlots.size(),
				Render.RenderData ? Render.RenderData->MaterialSlots.size() : 0), EStaticMeshBuildStage::Validation});
		std::unordered_set<FName> SlotNames;
		for (size_t Index = 0; Index < Render.MaterialSlots.size(); ++Index)
		{
			const auto& Slot = Render.MaterialSlots[Index];
			if (Slot.Name.IsNone() || !SlotNames.insert(Slot.Name).second)
				return Fail(FStaticMeshBuildFailure{std::format(
					"StaticMesh candidate requires unique named material slots (slot {}, name '{}').", Index, Slot.Name.ToString()), EStaticMeshBuildStage::Validation});
		}
		for (size_t Index = 0; Index < Render.RenderData->LODResources.size(); ++Index)
		{
			const auto& LOD = Render.RenderData->LODResources[Index];
			if (LOD.NumTexCoords > MaxStaticMeshUVChannels)
				return Fail(FStaticMeshBuildFailure{std::format(
					"StaticMesh candidate LOD {} has {} UV channels; maximum {}.", Index, LOD.NumTexCoords, MaxStaticMeshUVChannels), EStaticMeshBuildStage::Validation});
		}
		FStaticMeshBuildMemoryEstimate Memory{Control.MaximumWorkingSetBytes};
		if (!Memory.Add(1, 1024 * 1024) || !Memory.Add(Render.MaterialSlots.capacity(), 32768)
			|| !Memory.Add(Render.RenderData->LODResources.capacity(), sizeof(FStaticMeshLODResources)))
			return BudgetFailure("StaticMesh render metadata exceeds its reservation.", Memory);
		for (const auto& LOD : Render.RenderData->LODResources)
		{
			if (!Memory.Add(LOD.VertexBuffers.PositionVertexBuffer.GetPositions().capacity(), 512)
				|| !Memory.Add(LOD.IndexBuffer.GetIndices().capacity(), 192)
				|| !Memory.Add(LOD.Sections.capacity(), sizeof(FStaticMeshSection)))
				return BudgetFailure("StaticMesh predicted finalization working set exceeds its reservation.", Memory);
		}
		bool bCancelled = false;
		const std::function<bool()> ShouldCancel = [&] {
			bCancelled = bCancelled || Control.IsCancelled();
			return bCancelled;
		};
		FStaticMeshPayloadData Payload;
		if (const auto Result = MakeStaticMeshPayloadData(*Render.RenderData, Payload, ShouldCancel); !Result)
			return Fail(Result.error().Code == EStaticMeshPayloadError::Cancelled
				? FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Validation, FormatStaticMeshPayloadError(Result.error()))
				: FStaticMeshBuildFailure{FormatStaticMeshPayloadError(Result.error()), EStaticMeshBuildStage::Validation});
		if (const auto Policy = ValidateStaticMeshLODScreenSizes(Render.RenderData->LODResources); !Policy)
			return Fail(bCancelled
				? FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Validation, FormatStaticMeshLODPolicyError(Policy.error()))
				: FStaticMeshBuildFailure{FormatStaticMeshLODPolicyError(Policy.error()), EStaticMeshBuildStage::Validation});
		if (Control.IsCancelled()) return Fail(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Validation, "StaticMesh candidate build was cancelled."));
		if (!Render.RenderData->RecalculateBounds(ShouldCancel))
			return Fail(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Validation, "StaticMesh bounds build was cancelled."));
#if DURIN_WITH_EDITOR
		for (auto& LOD : Render.RenderData->LODResources)
		{
			LOD.RayQueryAcceleration = BuildStaticMeshRayQueryAcceleration(LOD, ShouldCancel);
			if (bCancelled) return Fail(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Validation, "StaticMesh ray build was cancelled."));
			// An unavailable optional acceleration retains exact reference traversal.
		}
#endif
		FStaticMeshBuildExecutionControl CollisionControl = Control;
		CollisionControl.ExpectedProviderRegistration = Render.ProviderRegistration;
		auto CollisionOutcome = FStaticMeshBuilder::BuildCollision(*Render.RenderData,
			Request.CollisionMode, Request.CollisionPolicy,
			Request.bPersistDerivedData, CollisionControl);
		if (!CollisionOutcome)
		{
			return Fail(CollisionOutcome.error());
		}
		Candidate->Collision = std::move(*CollisionOutcome);
		if (Request.CollisionMode != EBodySetupCollisionSourceMode::None
			&& (Render.Descriptor.ProducerIdentity != Candidate->Collision.Descriptor.ProducerIdentity
				|| Render.Descriptor.RenderBuilderVersion != Candidate->Collision.Descriptor.RenderBuilderVersion
				|| Render.Descriptor.CollisionBuilderVersion != Candidate->Collision.Descriptor.CollisionBuilderVersion))
			return Fail(FStaticMeshBuildFailure{"StaticMesh provider changed while constructing the combined candidate.", EStaticMeshBuildStage::Validation});
		if (Control.IsCancelled()) return Fail(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Validation, "StaticMesh candidate build was cancelled."));
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
		if (!bFits) return BudgetFailure("StaticMesh retained candidate exceeds its reservation.", Retained);
		Request.Source.ReleaseGeometry();
		Candidate->Request = std::move(Request);
		return Candidate;
	}

	FStaticMeshBuildFailure::FStaticMeshBuildFailure(std::string InMessage, EStaticMeshBuildStage InStage)
		: Message(InMessage.substr(0, MaximumStaticMeshBuildDiagnosticBytes)), Stage(InStage)
	{
	}

	auto FormatStaticMeshBuildMessages(std::span<const std::string> Messages) -> std::string
	{
		std::string Text;
		for (size_t Index = 0; Index < Messages.size(); ++Index)
		{
			if (Text.size() >= MaximumStaticMeshBuildDiagnosticBytes) break;
			if (Index != 0) Text += '\n';
			Text.append(Messages[Index], 0, MaximumStaticMeshBuildDiagnosticBytes - Text.size());
		}
		return Text;
	}

	auto FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage Stage, std::string Message) -> FStaticMeshBuildFailure
	{
		FStaticMeshBuildFailure Error(std::move(Message), Stage);
		Error.bCancelled = true;
		return Error;
	}

	auto DStaticMesh::Build(const FStaticMeshSource& InSource) -> std::expected<void, std::vector<std::string>>
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!IsValid(this))
			return std::unexpected(std::vector<std::string>{"StaticMesh build requires a valid owner."});
		if (!InSource.IsValid())
			return std::unexpected(std::vector<std::string>{"StaticMesh build requires valid canonical source metadata."});
		// Retire older work without pumping callbacks or waiting for unrelated compilation.
		CancelStaticMeshCompilation(*this);
		const auto Snapshot = FStaticMeshBuilder::Capture(*this);
		auto Candidate = FStaticMeshBuilder::BuildCandidate(FStaticMeshBuilder::MakeRequest(InSource, Snapshot));
		if (!Candidate)
			return std::unexpected(std::vector<std::string>{Candidate.error().ToString()});
		for (const auto& Warning : (*Candidate)->GetCacheErrors()) DURIN_WARN("{}", Warning.ToString());
		if (const auto Applied = FStaticMeshBuilder::ApplyCandidate(*this, std::move(*Candidate), Snapshot); !Applied)
			return std::unexpected(std::vector<std::string>{Applied.error().ToString()});
		return {};
	}

	auto DStaticMesh::Build(FStaticMeshDecodedGeometry Geometry) -> std::expected<void, std::vector<std::string>>
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		FStaticMeshSource InSource;
		if (const auto Initialized = InSource.Initialize(std::move(Geometry)); !Initialized)
			return std::unexpected(std::vector<std::string>{
				FormatStaticMeshSourceError(Initialized.error()).substr(0, MaximumStaticMeshBuildDiagnosticBytes)});
		return Build(InSource);
	}
}
