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
	auto FStaticMeshBuilder::Capture(const DStaticMesh& Mesh)
		-> FStaticMeshReconciliationSnapshot
	{
		return {.MaterialSlots = std::vector<FMeshMaterialSlotDefinition>(
				Mesh.GetMaterialSlots().begin(), Mesh.GetMaterialSlots().end()),
			.NormalizedSize = Mesh.GetNormalizedSize(),
			.SourceIdentity = Mesh.GetSource().GetIdentity()};
	}

	auto FStaticMeshBuilder::FinalizeRenderData(FStaticMeshRenderData& Render,
		const FAssetBuildTaskContext& Control) -> std::expected<void, FStaticMeshBuildFailure>
	{
		const auto Fail = [](FStaticMeshBuildFailure Error) { return std::unexpected(std::move(Error)); };
		const auto BudgetFailure = [&](std::string_view Reason, const FAssetBuildMemoryEstimate& Memory) {
			return Fail(FStaticMeshBuildFailure{std::format("{} Limit {}, accumulated {}, rejected {} x {} bytes.",
				Reason, Memory.Limit, Memory.Bytes, Memory.RejectedCount, Memory.RejectedWidth), EStaticMeshBuildStage::Validation});
		};
		if (Render.MaterialSlots.empty()
			|| Render.MaterialSlots.size() > MaximumMeshMaterialSlots)
			return Fail(FStaticMeshBuildFailure{std::format(
				"StaticMesh render material-slot count {} is invalid.", Render.MaterialSlots.size()), EStaticMeshBuildStage::Validation});
		std::unordered_set<std::string> SlotNames;
		for (size_t Index = 0; Index < Render.MaterialSlots.size(); ++Index)
		{
			const auto& Slot = Render.MaterialSlots[Index];
			if (Slot.Name.empty() || !SlotNames.insert(Slot.Name).second)
				return Fail(FStaticMeshBuildFailure{std::format(
					"StaticMesh render data requires unique named material slots (slot {}, name '{}').", Index, Slot.Name), EStaticMeshBuildStage::Validation});
		}
		for (size_t Index = 0; Index < Render.LODResources.size(); ++Index)
		{
			const auto& LOD = Render.LODResources[Index];
			if (LOD.NumTexCoords > MaxStaticMeshUVChannels)
				return Fail(FStaticMeshBuildFailure{std::format(
					"StaticMesh render LOD {} has {} UV channels; maximum {}.", Index, LOD.NumTexCoords, MaxStaticMeshUVChannels), EStaticMeshBuildStage::Validation});
		}
		FAssetBuildMemoryEstimate Memory{Control.MaximumWorkingSetBytes};
		if (!Memory.Add(1, 1024 * 1024) || !Memory.Add(Render.MaterialSlots.capacity(), 32768)
			|| !Memory.Add(Render.LODResources.capacity(), sizeof(FStaticMeshLODResources)))
			return BudgetFailure("StaticMesh render metadata exceeds its reservation.", Memory);
		for (const auto& LOD : Render.LODResources)
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
		if (!Render.RecalculateBounds(ShouldCancel))
			return Fail(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Validation, "StaticMesh bounds build was cancelled."));
		FStaticMeshPayloadData Payload;
		if (const auto Result = MakeStaticMeshPayloadData(Render, Payload, ShouldCancel); !Result)
			return Fail(Result.error().Code == EStaticMeshPayloadError::Cancelled
				? FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Validation, FormatStaticMeshPayloadError(Result.error()))
				: FStaticMeshBuildFailure{FormatStaticMeshPayloadError(Result.error()), EStaticMeshBuildStage::Validation});
		if (const auto Policy = ValidateStaticMeshLODScreenSizes(Render.LODResources); !Policy)
			return Fail(bCancelled
				? FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Validation, FormatStaticMeshLODPolicyError(Policy.error()))
				: FStaticMeshBuildFailure{FormatStaticMeshLODPolicyError(Policy.error()), EStaticMeshBuildStage::Validation});
		if (Control.IsCancelled()) return Fail(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Validation, "StaticMesh candidate build was cancelled."));
#if DURIN_WITH_EDITOR
		for (auto& LOD : Render.LODResources)
		{
			LOD.RayQueryAcceleration = BuildStaticMeshRayQueryAcceleration(LOD, ShouldCancel);
			if (bCancelled) return Fail(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Validation, "StaticMesh ray build was cancelled."));
			// An unavailable optional acceleration retains exact reference traversal.
		}
#endif
		return {};
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

	auto DStaticMesh::Build(const FStaticMeshSource& InSource,
		std::optional<std::vector<FMeshMaterialSlotDefinition>> PreparedMaterialSlots) -> std::expected<void, std::vector<std::string>>
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!IsValid(this))
			return std::unexpected(std::vector<std::string>{"StaticMesh build requires a valid owner."});
		if (!InSource.IsValid())
			return std::unexpected(std::vector<std::string>{"StaticMesh build requires valid canonical source metadata."});
		// Retire older work without pumping callbacks or waiting for unrelated compilation.
		CancelStaticMeshCompilation(*this);
		const auto Snapshot = FStaticMeshBuilder::Capture(*this);
		auto Input = Snapshot;
		if (PreparedMaterialSlots) Input.MaterialSlots = *PreparedMaterialSlots;
		std::vector<FAssetBuildCacheWarning> Warnings;
		auto Render = FStaticMeshBuilder::Build({.Reconciliation = Input, .Source = InSource}, {}, &Warnings);
		if (!Render) return std::unexpected(std::vector<std::string>{Render.error().ToString()});
		for (const auto& Warning : Warnings) DURIN_WARN("StaticMesh {}", Warning.ToString());
		if (const auto Applied = CommitStaticMeshBuild(*this, std::move(*Render), InSource, Snapshot, true, {}, nullptr,
			PreparedMaterialSlots ? &*PreparedMaterialSlots : nullptr); !Applied)
			return std::unexpected(std::vector<std::string>{Applied.error().ToString()});
		return {};
	}

	auto DStaticMesh::Build(FStaticMeshDecodedGeometry Geometry,
		std::optional<std::vector<FMeshMaterialSlotDefinition>> PreparedMaterialSlots) -> std::expected<void, std::vector<std::string>>
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		FStaticMeshSource InSource;
		if (const auto Initialized = InSource.Initialize(std::move(Geometry)); !Initialized)
			return std::unexpected(std::vector<std::string>{
				FormatStaticMeshSourceError(Initialized.error()).substr(0, MaximumStaticMeshBuildDiagnosticBytes)});
		return Build(InSource, std::move(PreparedMaterialSlots));
	}
}
