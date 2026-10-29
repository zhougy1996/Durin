#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/IMeshBuilderModule.h"
#include "StaticMeshBuildPrivate.h"
#include "Asset/AssetBuildTaskContext.h"
#include "StaticMesh/StaticMeshCompilationTestSupport.h"

#if DURIN_WITH_EDITOR
#include "DerivedDataBuildSession.h"
#include "Asset/ResourceBuildSessions.h"
#endif
#include "StaticMeshSharedOutput.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshDerivedDataKey.h"

namespace Durin
{
#if DURIN_WITH_EDITOR
	namespace
	{
		AssetPrivate::FResourceBuildSessions Sessions;
		std::mutex CompletionHookMutex;
		std::function<void()> CompletionHookForTests;

		auto CheckRenderPreparationBudget(const FStaticMeshRenderData& Render, uint64 MaximumWorkingSetBytes)
			-> std::expected<void, std::string>
		{
			const auto Fail = [](std::string Error) { return std::unexpected(std::move(Error)); };
			const auto BudgetFailure = [&](std::string_view Reason, const FAssetBuildMemoryEstimate& Memory) {
				return Fail(MakeAssetBuildDiagnostic(std::format("{} Limit {}, accumulated {}, rejected {} x {} bytes.",
					Reason, Memory.Limit, Memory.Bytes, Memory.RejectedCount, Memory.RejectedWidth)));
			};
			FAssetBuildMemoryEstimate Memory{MaximumWorkingSetBytes};
			if (!Memory.Add(1, 1024 * 1024) || !Memory.Add(Render.MaterialSlots.capacity(), 32768)
				|| !Memory.Add(Render.LODResources.capacity(), sizeof(FStaticMeshLODResources)))
				return BudgetFailure("StaticMesh render metadata exceeds its reservation.", Memory);
			for (const auto& LOD : Render.LODResources)
			{
				if (!Memory.Add(LOD.VertexBuffers.PositionVertexBuffer.GetPositionCapacity(), 512)
					|| !Memory.Add(LOD.IndexBuffer.GetIndicesCapacity(), 192)
					|| !Memory.Add(LOD.Sections.capacity(), sizeof(FStaticMeshSection)))
					return BudgetFailure("StaticMesh render preparation working set exceeds its reservation.", Memory);
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

#if DURIN_WITH_EDITORONLY_DATA && DURIN_WITH_EDITOR
	namespace StaticMeshPrivate
	{
		auto ValidateRenderRequest(const FStaticMeshBuildRequest& Request, uint64 MaximumWorkingSetBytes)
			-> std::expected<void, std::string>
		{
			if (!Request.Source.IsValid() || !std::isfinite(Request.Settings.NormalizedSize)
				|| Request.Settings.NormalizedSize <= 0)
				return std::unexpected(MakeAssetBuildDiagnostic("StaticMesh render source or normalization is invalid."));
			FAssetBuildMemoryEstimate SourceMemory{MaximumWorkingSetBytes};
			if (!SourceMemory.Add(Request.Source.GetGeometryBulk().GetPayloadSize(), 8)
				|| !SourceMemory.Add(Request.Source.GetMeshCount(), sizeof(FMeshDescriptionSection))
				|| !SourceMemory.Add(uint64(Request.Source.GetMaterialSlotCount()) * Request.Source.GetNumLODs(), 32768)
				|| !SourceMemory.Add(Request.Settings.MaterialSlots.size(), 32768))
				return std::unexpected(MakeAssetBuildDiagnostic("StaticMesh decoded source exceeds its reservation."));
			if (Request.Settings.MaterialSlots.size() > MaximumMeshMaterialSlots)
				return std::unexpected(MakeAssetBuildDiagnostic("StaticMesh material-slot input exceeds the slot limit."));
			std::unordered_set<FName> SlotNames;
			std::unordered_set<uint32> SourceIndices;
			for (const auto& Slot : Request.Settings.MaterialSlots)
			{
				if (Slot.Name.IsNone() || !SlotNames.insert(Slot.Name).second
					|| Slot.SourceName.size() > 4096 || !SourceIndices.insert(Slot.SourceMaterialIndex).second
					)
					return std::unexpected(MakeAssetBuildDiagnostic("StaticMesh requires bounded, uniquely named material slots with unambiguous source indices."));
			}
			return {};
		}
		auto MakeRenderOptions(bool bPersist, uint64 MaximumWorkingSetBytes, FCancellationToken Cancellation = {})
			-> DerivedData::FBuildRequestOptions
		{
			DerivedData::FBuildRequestOptions Options;
			Options.Policy.StoreOnBuild = bPersist;
			Options.Policy.MaximumWorkingSetBytes = MaximumWorkingSetBytes;
			Options.Policy.InputLimits.MaximumTotalBytes = MaximumStaticMeshSourceBytes + 64ull * 1024 * 1024;
			Options.Policy.OutputLimits.MaximumTotalBytes = MaximumStaticMeshPayloadBytes;
			const uint64 MaximumBytes = std::min(MaximumStaticMeshPayloadBytes, MaximumWorkingSetBytes / 16);
			Options.Policy.PersistenceLimits.MaximumTotalBytes = MaximumBytes;
			Options.Policy.MaximumEncodedBytes = MaximumBytes;
			Options.Cancellation = std::move(Cancellation);
			return Options;
		}
		auto ConsumeRenderBuild(const FStaticMeshBuildSettings& Settings, DerivedData::FBuildCompleteParams Completion,
			uint64 MaximumWorkingSetBytes) -> FRenderBuildResult
		{
			if (Completion.GetStatus() != DerivedData::EStatus::Ok)
			{
				std::string Description = "StaticMesh derived-data build failed.";
				if (!Completion.GetDiagnostic().empty()) Description = Completion.GetDiagnostic();
				if (const auto* Output = Completion.GetOutput(); Output && !Output->GetMessages().empty()) Description = Output->GetMessages().back().Text;
				else if (const auto* Output = Completion.GetOutput(); Output && !Output->GetLogs().empty()) Description = Output->GetLogs().back().Text;
				return std::unexpected(MakeAssetBuildDiagnostic(std::move(Description)));
			}
			auto Product = StaticMeshPrivate::AssembleSharedOutput(*Completion.GetOutput());
			if (!Product) return std::unexpected(MakeAssetBuildDiagnostic(std::move(Product.error())));
			if (auto Metadata = RestoreRuntimeMetadata(Settings.MaterialSlots, **Product); !Metadata)
				return std::unexpected(MakeAssetBuildDiagnostic(std::move(Metadata.error())));
			if (auto Budget = CheckRenderPreparationBudget(**Product, MaximumWorkingSetBytes); !Budget) return std::unexpected(std::move(Budget.error()));
			return std::move(*Product);
		}
		auto DispatchRenderBuild(FStaticMeshBuildRequest& Request, uint64 BuilderVersion,
			DerivedData::IRequestOwner& Owner, FCancellationToken Cancellation, uint64 MaximumWorkingSetBytes,
			DerivedData::FBuildCompletionCallback Completion) -> std::expected<void, std::string>
		{
			if (Cancellation.IsCancelled()) return {};
			if (auto Validation = ValidateRenderRequest(Request, MaximumWorkingSetBytes); !Validation) return Validation;
			auto Definition = MakeStaticMeshSessionDefinition(uint32(Request.Settings.MaterialSlots.size()), BuilderVersion);
			if (!Definition) return std::unexpected(MakeAssetBuildDiagnostic("StaticMesh build definition is invalid."));
			auto Options = MakeRenderOptions(Request.bPersistDerivedData, MaximumWorkingSetBytes, std::move(Cancellation));
			Options.InputResolver = MakeRenderInputResolver(Request);
			Request.Source.ReleaseGeometry();
			auto Session = Sessions.Get(DerivedData::EBuildCacheExecution::Task);
			if (!Session) return std::unexpected(MakeAssetBuildDiagnostic(Session.error().Description));
			std::function<void()> CompletionHook;
			{
				std::lock_guard Lock(CompletionHookMutex);
				CompletionHook = CompletionHookForTests;
			}
			if (CompletionHook)
			{
				Completion = [Callback = std::move(Completion), Hook = std::move(CompletionHook)](auto Value) {
					const bool bSuccess = Value.GetStatus() == DerivedData::EStatus::Ok;
					Callback(std::move(Value));
					if (bSuccess) Hook();
				};
			}
			auto Admitted = (*Session)->Build(std::move(*Definition), Owner, std::move(Completion), {}, std::move(Options));
			if (!Admitted) return std::unexpected(MakeAssetBuildDiagnostic(Admitted.error().Description));
			return {};
		}
	}
	namespace StaticMeshPrivate
	{
		auto OpenBuildSessions() -> void { Sessions.Open(); }
		auto ShutdownBuildSessions() -> void
		{ require(Sessions.Close() == DerivedData::EBuildDrainResult::Drained); }
	}
	namespace AssetPrivate
	{
		auto SetStaticMeshBuildCompletionHookForTests(std::function<void()> Hook) -> void
		{
			std::lock_guard Lock(CompletionHookMutex);
			CompletionHookForTests = std::move(Hook);
		}
		auto SetStaticMeshAsyncCacheForTests(std::shared_ptr<DerivedData::ICache> Cache) -> void
		{
			std::shared_ptr<DerivedData::IBuild> Service;
			if (Cache)
			{
				Service = DerivedData::CreateBuild({.Cache = std::move(Cache)});
				require(Service->Register(StaticMeshPrivate::MakeRenderBuildFunction()).has_value());
			}
			require(Sessions.SetTaskService(std::move(Service)) == DerivedData::EBuildDrainResult::Drained);
		}
	}
#endif

#if DURIN_WITH_EDITORONLY_DATA
	auto BuildStaticMeshRenderData(FStaticMeshBuildRequest Request,
		uint64 MaximumWorkingSetBytes) -> std::expected<std::unique_ptr<FStaticMeshRenderData>, std::string>
	{
#if DURIN_WITH_EDITOR
		if (auto Validation = StaticMeshPrivate::ValidateRenderRequest(Request, MaximumWorkingSetBytes); !Validation)
			return std::unexpected(std::move(Validation.error()));
#endif
#if !DURIN_WITH_EDITOR
		return std::unexpected(MakeAssetBuildDiagnostic("StaticMesh build orchestration is unavailable outside editor builds."));
#else
		const auto* Module = IMeshBuilderModule::Get();
		if (!Module) return std::unexpected(MakeAssetBuildDiagnostic("The MeshBuilder module is unavailable."));
		auto Definition = MakeStaticMeshSessionDefinition(uint32(Request.Settings.MaterialSlots.size()), Module->GetBuildVersion());
		if (!Definition) return std::unexpected(MakeAssetBuildDiagnostic(
			"StaticMesh build definition is invalid."));
		auto Inputs = StaticMeshPrivate::MakeRenderInputResolver(Request);
		// The resolver retains canonical bulk, not the request's optional decoded residency.
		Request.Source.ReleaseGeometry();
		auto Options = StaticMeshPrivate::MakeRenderOptions(Request.bPersistDerivedData, MaximumWorkingSetBytes);
		auto Built = Sessions.Build(std::move(*Definition), std::move(Inputs), std::move(Options));
		if (!Built) return std::unexpected(MakeAssetBuildDiagnostic(
			Built.error().Description));
		return StaticMeshPrivate::ConsumeRenderBuild(Request.Settings, std::move(*Built), MaximumWorkingSetBytes);
#endif
	}
#endif

}
