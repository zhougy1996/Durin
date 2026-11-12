#include "Shader/ShaderCompilingManager.h"
#include "Asset/Asset.h"
#include "Materials/Material.h"
#include "Materials/ObjectCacheContext.h"
#include "Materials/MaterialShaderMap.h"
#include "Materials/MaterialPreparationQueue.h"
#include "Shader/ShaderMapPreparation.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		std::atomic_uint8_t GPendingShaderReloadMode = 0;
		auto CheckMaterialCompileGameThread() -> void { if (GIsGameThreadIdInitialized) CheckGameThread(); }
	}
	namespace Private
	{
		auto GetMaterialPreparationQueue() -> FMaterialPreparationQueue&
		{
			CheckMaterialCompileGameThread();
			static FMaterialPreparationQueue Queue;
			return Queue;
		}
	}

	auto NotifyMaterialShaderReload(bool bForceRecompile) -> void
	{
		uint8 Desired = bForceRecompile ? 2 : 1;
		uint8 Current = GPendingShaderReloadMode.load(std::memory_order_acquire);
		while (Current < Desired
			&& !GPendingShaderReloadMode.compare_exchange_weak(
				Current, Desired, std::memory_order_acq_rel)) {}
	}

	auto FShaderCompilingManager::Start() -> FAssetCompilerStartResult
	{
		if (!IsTaskSchedulerRunning()) return {EAssetCompilerStartError::SchedulerUnavailable};
		Accepting = true; return {};
	}
	auto FShaderCompilingManager::StopAdmission() -> void
	{
		{ std::lock_guard Lock(JobMutex); Accepting = false; }
		auto& EditQueue = Private::GetMaterialEditCompileQueue();
		EditQueue.Process(EditQueue.Num(), [](FWeakObjectPtr Owner) {
			if (auto* Material = Cast<DMaterialInterface>(Owner.Get()); IsValid(Material))
				Material->CompilationState.EditCompileDeadline.reset();
			return false;
		});
	}
	auto FShaderCompilingManager::GetNumRemainingAssets() const -> uint64
	{
		return GetMaterialDiagnostics().OutstandingConsumerCount
			+ Private::GetMaterialPreparationQueue().Num() + Private::GetMaterialEditCompileQueue().Num()
			+ GetNumRemainingShaderJobs() + GetNumRemainingMaterialShaderMaps();
	}
	auto FShaderCompilingManager::ProcessAsyncTasks(const FAssetCompileProcessParams& Params)
		-> FAssetCompileProcessResult
	{ return ProcessAsyncResults(Params); }
	auto FShaderCompilingManager::FinishCompilationForObjects(std::span<DObject* const> Objects)
		-> FAssetCompileProcessResult
	{
		FAssetCompileProcessResult Aggregate;
		std::vector<FWeakObjectPtr> Owners;
		for (DObject* Object : Objects)
			if (auto* Material = Cast<DMaterialInterface>(Object); IsValid(Material))
			{
				if (Material->GetMaterialCompileStatus().HasScheduledEdits())
					Private::FMaterialCompilationLifecycle::RequestCurrent(*Material, false);
				Owners.push_back(FWeakObjectPtr(Material));
			}
		while (std::ranges::any_of(Owners, [this](FWeakObjectPtr Owner) {
			auto* Material = Cast<DMaterialInterface>(Owner.Get());
			return IsValid(Material) && Material->IsCompiling();
		}))
		{
			RaiseMaterialPriority(Owners);
			auto Item = ProcessAsyncResults({.MaximumCompletions = std::numeric_limits<uint32>::max()});
			Aggregate.ProcessedCompletionCount += Item.ProcessedCompletionCount;
			Aggregate.SuccessfullyCompiledAssets.insert(
				Aggregate.SuccessfullyCompiledAssets.end(),
				Item.SuccessfullyCompiledAssets.begin(),
				Item.SuccessfullyCompiledAssets.end());
			if (Item.ProcessedCompletionCount == 0) std::this_thread::yield();
		}
		FinishMaterialCompilation(Owners);
		for (auto Owner : Owners)
			if (auto* Material = Cast<DMaterialInterface>(Owner.Get()); IsValid(Material))
				(void)Material->PumpFeatureLevelResources(true);
		return Aggregate;
	}
	auto FShaderCompilingManager::MarkCompilationAsCanceled(std::span<DObject* const> Objects)
		-> void
	{
		CheckMaterialCompileGameThread();
		for (DObject* Object : Objects)
			if (auto* Material = Cast<DMaterialInterface>(Object); IsValid(Material))
			{
				const bool bCanceled = CancelCompilation(FWeakObjectPtr(Material));
				if (bCanceled || Material->CompilationState.CookedPreparation)
					Private::FMaterialCompilationLifecycle::MarkCanceled(*Material);
				else
				{
					// Withdrawing an unsubmitted edit is not a request outcome.
					Material->CompilationState.EditCompileDeadline.reset();
					Private::GetMaterialEditCompileQueue().Remove(FWeakObjectPtr(Material));
					Material->CancelFeatureLevelCompilation();
					Private::GetMaterialPreparationQueue().Remove(FWeakObjectPtr(Material));
				}
			}
	}
	auto FShaderCompilingManager::ProcessAsyncResults(const FAssetCompileProcessParams& Params) -> FAssetCompileProcessResult
	{
		CheckMaterialCompileGameThread();
		if (!Params.MaximumCompletions || (Params.Deadline && std::chrono::steady_clock::now() >= *Params.Deadline)) return {};
		PollMaterialCancellation();
		CleanupFinishedShaderMaps();
		const uint8 ReloadMode = GPendingShaderReloadMode.exchange(
			0, std::memory_order_acq_rel);
		if (ReloadMode != 0)
		{
			const std::vector<DObject*> Objects =
				GDObjectArray.Snapshot(EObjectQueryScope::LiveOnly);
			for (DObject* Object : Objects)
				if (auto* Material = Cast<DMaterialInterface>(Object); IsValid(Material))
				{
					const FMaterialRenderProxyRef Proxy =
						Material->GetRenderProxy();
					if ((Proxy && Proxy->GetRefCount() > 2) || Material->HasFeatureLevelRenderReferences())
						RequestMaterialRecompile(*Material, ReloadMode > 1);
				}
		}

		FAssetCompileProcessResult Processed;
		FObjectCacheContext CompletionContext;
		Processed.ProcessedCompletionCount = ProcessMaterialResults(Params, [&](FMaterialCompileResult Result) {
			DObject* Object = Result.Owner.Get();
			if (auto* Material = Cast<DMaterialInterface>(Object); IsValid(Material)
				&& Private::FMaterialCompilationLifecycle::Admit(*Material, std::move(Result), &CompletionContext))
				Processed.SuccessfullyCompiledAssets.emplace_back(Material);
		});
		if (!Params.Deadline || std::chrono::steady_clock::now() < *Params.Deadline) Private::GetMaterialPreparationQueue().Process(
			Private::MaterialPreparationMaxChecks, [this, &Params, &Processed](FWeakObjectPtr Owner) {
				auto* Material = Cast<DMaterialInterface>(Owner.Get());
				if (!IsValid(Material)) return false;
				const bool FeaturePending = Material->PumpFeatureLevelResources();
				if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
				{
					if (Processed.ProcessedCompletionCount >= Params.MaximumCompletions
						|| (Params.Deadline && std::chrono::steady_clock::now() >= *Params.Deadline)) return true;
					if (Material->PollCookedProgramPreparation()) return true;
					++Processed.ProcessedCompletionCount;
					if (Material->IsComplete()) Processed.SuccessfullyCompiledAssets.emplace_back(Material);
					return FeaturePending;
				}
				return FeaturePending;
			});
		if (!Params.Deadline || std::chrono::steady_clock::now() < *Params.Deadline)
			Private::GetMaterialEditCompileQueue().Process(Private::MaterialPreparationMaxChecks, [&](FWeakObjectPtr Owner) {
				auto* Material = Cast<DMaterialInterface>(Owner.Get());
				if (!IsValid(Material)) return false;
				if (Params.Deadline && std::chrono::steady_clock::now() >= *Params.Deadline) return true;
				if (IsAcceptingMaterialRequests()) Private::FMaterialCompilationLifecycle::SubmitScheduledEdit(*Material);
				else Material->CompilationState.EditCompileDeadline.reset();
				return Material->GetMaterialCompileStatus().HasScheduledEdits();
			});
		return Processed;
	}
	auto FShaderCompilingManager::FinishAllCompilation() -> FAssetCompileProcessResult
	{
		FAssetCompileProcessResult Aggregate;
		for (;;) {
			RaiseMaterialPriority();
			FinishMaterialCompilation();
			FinishShaderJobs();
			auto Item = ProcessAsyncResults({.MaximumCompletions = std::numeric_limits<uint32>::max()});
			Aggregate.ProcessedCompletionCount += Item.ProcessedCompletionCount;
			Aggregate.SuccessfullyCompiledAssets.insert(Aggregate.SuccessfullyCompiledAssets.end(),
				Item.SuccessfullyCompiledAssets.begin(), Item.SuccessfullyCompiledAssets.end());
			if (GetNumRemainingAssets() == 0) break;
		}
		return Aggregate;
	}
	auto FShaderCompilingManager::Shutdown() -> void
	{
		StopAdmission();
		auto& Resources = Private::GetMaterialPreparationQueue();
		Resources.Process(Resources.Num(), [](FWeakObjectPtr Owner) {
			if (auto* Material = Cast<DMaterialInterface>(Owner.Get()); IsValid(Material))
				Private::FMaterialCompilationLifecycle::MarkCanceled(*Material);
			return false;
		});
		CancelAllCompilation();
		FinishMaterialCompilation();
		FinishShaderJobs();
		{
			std::lock_guard Lock(MaterialMutex);
			MaterialDiagnostics.CanceledRequests += OutstandingMaterialConsumers;
			RequestedShaderMaps.clear(); MaterialRequests.clear(); MaterialResults.clear(); OutstandingMaterialConsumers = 0;
		}
		FMaterialShaderMap::ClearCache();
		ClearShaderMapPreparations();
	}
	auto FShaderCompilingManager::IsAcceptingMaterialRequests() const -> bool
	{
		return Accepting && IsTaskSchedulerRunning() && FAssetCompilingManager::Get().IsAcceptingRequests();
	}
}
