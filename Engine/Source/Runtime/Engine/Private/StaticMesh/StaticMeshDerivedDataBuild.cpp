#include "StaticMesh/StaticMeshBuild.h"

#if DURIN_WITH_EDITOR
#include "DerivedDataBuildSession.h"
#endif
#include "StaticMeshBuildFunction.h"
#include "StaticMeshSharedOutput.h"
#include "StaticMeshRenderPreparation.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshDerivedDataKey.h"

namespace Durin
{
#if DURIN_WITH_EDITOR
	namespace
	{
		std::mutex BuildSessionMutex;
		std::shared_ptr<DerivedData::FBuildSession> BuildSession;

		auto GetBuildSession() -> std::shared_ptr<DerivedData::FBuildSession>
		{
			std::lock_guard Lock(BuildSessionMutex);
			if (!BuildSession)
			{
				auto Created = DerivedData::GetBuild().CreateSession();
				if (!Created) return {};
				BuildSession = std::move(*Created);
			}
			return BuildSession;
		}

		auto BuildRenderDefinition(DerivedData::FBuildDefinition Definition,
			std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver,
			DerivedData::FBuildRequestOptions Options)
			-> std::optional<DerivedData::FBuildCompleteParams>
		{
			if (Options.Cancellation.IsCancelled()) return DerivedData::FBuildCompleteParams::Canceled(
				std::nullopt, DerivedData::EBuildStatus::None);
			auto Session = GetBuildSession();
			if (!Session) return std::nullopt;
			Options.InputResolver = std::move(Resolver);
			DerivedData::FBuildRequestOwner Owner;
			std::optional<DerivedData::FBuildCompleteParams> Completion;
			auto Admitted = Session->Build(std::move(Definition), Owner, [&](auto Value) {
				Completion = std::move(Value);
			}, {}, std::move(Options));
			if (!Admitted) return std::nullopt;
			require(Owner.Wait() == DerivedData::EBuildWaitResult::Completed);
			if (!Completion) return std::nullopt;
			return std::move(*Completion);
		}

		auto CheckRenderPreparationBudget(const FStaticMeshRenderData& Render, const FAssetBuildTaskContext& Control)
			-> std::expected<void, FStaticMeshBuildFailure>
		{
			const auto Fail = [](FStaticMeshBuildFailure Error) { return std::unexpected(std::move(Error)); };
			const auto BudgetFailure = [&](std::string_view Reason, const FAssetBuildMemoryEstimate& Memory) {
				return Fail(FStaticMeshBuildFailure{std::format("{} Limit {}, accumulated {}, rejected {} x {} bytes.",
					Reason, Memory.Limit, Memory.Bytes, Memory.RejectedCount, Memory.RejectedWidth), EStaticMeshBuildStage::Validation});
			};
			FAssetBuildMemoryEstimate Memory{Control.MaximumWorkingSetBytes};
			if (!Memory.Add(1, 1024 * 1024) || !Memory.Add(Render.MaterialSlots.capacity(), 32768)
				|| !Memory.Add(Render.LODResources.capacity(), sizeof(FStaticMeshLODResources)))
				return BudgetFailure("StaticMesh render metadata exceeds its reservation.", Memory);
			for (const auto& LOD : Render.LODResources)
			{
				if (!Memory.Add(LOD.VertexBuffers.PositionVertexBuffer.GetPositionCapacity(), 512)
					|| !Memory.Add(LOD.IndexBuffer.GetIndicesCapacity(), 192)
					|| !Memory.Add(LOD.Sections.capacity(), sizeof(FStaticMeshSection)))
					return BudgetFailure("StaticMesh predicted ray preparation working set exceeds its reservation.", Memory);
			}
			return {};
		}

		auto RestoreRuntimeMetadata(
			std::span<const FStaticMeshBuildMaterialSlot> MaterialSlots,
			FStaticMeshRenderData& RenderData) -> std::expected<void, std::string>
		{
			if (RenderData.MaterialSlots.size() != MaterialSlots.size())
			{
				return std::unexpected(std::format("Cached material slot count {} does not match {}.", RenderData.MaterialSlots.size(), MaterialSlots.size()));
			}
			for (size_t SlotIndex = 0; SlotIndex < MaterialSlots.size(); ++SlotIndex)
			{
				RenderData.MaterialSlots[SlotIndex].Name =
					MaterialSlots[SlotIndex].Name.ToString();
				RenderData.MaterialSlots[SlotIndex].SourceMaterialIndex =
					MaterialSlots[SlotIndex].SourceMaterialIndex;
			}
			for (size_t LODIndex = 0; LODIndex < RenderData.LODResources.size(); ++LODIndex)
				for (size_t SectionIndex = 0;
					SectionIndex < RenderData.LODResources[LODIndex].Sections.size();
					++SectionIndex)
					RenderData.LODResources[LODIndex].Sections[SectionIndex].Name =
						std::format("LOD{}_Section{}", LODIndex, SectionIndex);
			return {};
		}

	}

#endif

#if DURIN_WITH_EDITORONLY_DATA
	auto BuildStaticMeshRenderData(FStaticMeshBuildRequest Request,
		const FAssetBuildTaskContext& Control) -> std::expected<std::unique_ptr<FStaticMeshRenderData>, FStaticMeshBuildFailure>
	{
		if (Control.IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Render));
		if (!Request.Source.IsValid() || !std::isfinite(Request.Settings.NormalizedSize)
			|| Request.Settings.NormalizedSize <= 0)
			return std::unexpected(FStaticMeshBuildFailure{"StaticMesh render source or normalization is invalid.", EStaticMeshBuildStage::Source});
		FAssetBuildMemoryEstimate SourceMemory{Control.MaximumWorkingSetBytes};
		if (!SourceMemory.Add(Request.Source.GetGeometryBulk().GetPayloadSize(), 8)
			|| !SourceMemory.Add(Request.Source.GetMeshCount(), sizeof(FMeshDescriptionSection))
			|| !SourceMemory.Add(Request.Source.GetMaterialSlotCount(), 32768))
			return std::unexpected(FStaticMeshBuildFailure{"StaticMesh decoded source exceeds its reservation.", EStaticMeshBuildStage::Validation});
		if (Request.Settings.MaterialSlots.size() > MaximumMeshMaterialSlots)
			return std::unexpected(FStaticMeshBuildFailure{"StaticMesh material-slot input exceeds the slot limit.", EStaticMeshBuildStage::Render});
		std::unordered_set<FName> SlotNames;
		std::unordered_set<uint32> SourceIndices;
		for (const auto& Slot : Request.Settings.MaterialSlots)
		{
			if (Slot.Name.IsNone() || !SlotNames.insert(Slot.Name).second
#if DURIN_WITH_EDITORONLY_DATA
				|| Slot.SourceName.size() > 4096 || !SourceIndices.insert(Slot.SourceMaterialIndex).second
#endif
				)
				return std::unexpected(FStaticMeshBuildFailure{"StaticMesh requires bounded, uniquely named material slots with unambiguous source indices.", EStaticMeshBuildStage::Render});
		}
		bool bCancelled = false;
		const auto IsCancelled = [&] {
			bCancelled = bCancelled || Control.IsCancelled();
			return bCancelled;
		};
		if (IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Render));
#if !DURIN_WITH_EDITOR
		return std::unexpected(FStaticMeshBuildFailure{"StaticMesh build orchestration is unavailable outside editor builds.", EStaticMeshBuildStage::Render});
#else
		const auto* Module = IMeshBuilderModule::Get();
		if (!Module) return std::unexpected(FStaticMeshBuildFailure{"The MeshBuilder module is unavailable.", EStaticMeshBuildStage::Render});
		auto Definition = MakeStaticMeshSessionDefinition(uint32(Request.Settings.MaterialSlots.size()), Module->GetBuildVersion());
		if (!Definition) return std::unexpected(FStaticMeshBuildFailure{
			"StaticMesh build definition is invalid.", EStaticMeshBuildStage::Source});
		auto Inputs = StaticMeshPrivate::MakeRenderInputResolver(Request);
		// The resolver retains canonical bulk, not the request's optional decoded residency.
		Request.Source.ReleaseGeometry();
		DerivedData::FBuildRequestOptions Options;
		Options.Policy.StoreOnBuild = Request.bPersistDerivedData;
		Options.Policy.MaximumWorkingSetBytes = Control.MaximumWorkingSetBytes;
		Options.Policy.InputLimits.MaximumTotalBytes = MaximumStaticMeshSourceBytes + 64ull * 1024 * 1024;
		Options.Policy.OutputLimits.MaximumTotalBytes = MaximumStaticMeshPayloadBytes;
		const uint64 MaximumBytes = std::min(MaximumStaticMeshPayloadBytes, Control.MaximumWorkingSetBytes / 16);
		Options.Policy.PersistenceLimits.MaximumTotalBytes = MaximumBytes;
		Options.Policy.MaximumEncodedBytes = MaximumBytes;
		Options.Cancellation = DerivedData::FBuildCancellation(IsCancelled);
		auto Built = BuildRenderDefinition(std::move(*Definition), std::move(Inputs), std::move(Options));
		if (!Built) return std::unexpected(FStaticMeshBuildFailure{
			"StaticMesh build session is unavailable.", EStaticMeshBuildStage::Render});
		auto& Completion = *Built;
		if (IsCancelled() || Completion.GetStatus() == DerivedData::EStatus::Canceled)
			return std::unexpected(FStaticMeshBuildFailure::Cancelled());
		if (Completion.GetStatus() == DerivedData::EStatus::Error)
		{
			std::string Description = "StaticMesh derived-data build failed.";
			if (const auto* Output = Completion.GetOutput(); Output && !Output->GetMessages().empty()) Description = Output->GetMessages().back().Text;
			else if (const auto* Output = Completion.GetOutput(); Output && !Output->GetLogs().empty()) Description = Output->GetLogs().back().Text;
			return std::unexpected(FStaticMeshBuildFailure{std::move(Description), EStaticMeshBuildStage::Render});
		}
		auto Product = StaticMeshPrivate::AssembleSharedOutput(*Completion.GetOutput(), IsCancelled);
		if (IsCancelled()) return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Validation));
		if (!Product) return std::unexpected(FStaticMeshBuildFailure{std::move(Product.error()), EStaticMeshBuildStage::Validation});
		if (auto Metadata = RestoreRuntimeMetadata(Request.Settings.MaterialSlots, **Product); !Metadata)
			return std::unexpected(FStaticMeshBuildFailure{std::move(Metadata.error()), EStaticMeshBuildStage::Validation});
		if (auto Budget = CheckRenderPreparationBudget(**Product, Control); !Budget) return std::unexpected(std::move(Budget.error()));
		if (!StaticMeshPrivate::PrepareRenderRayQueries(**Product, IsCancelled))
			return std::unexpected(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Validation, "StaticMesh ray build was cancelled."));
		return std::move(*Product);
#endif
	}
#endif

}
