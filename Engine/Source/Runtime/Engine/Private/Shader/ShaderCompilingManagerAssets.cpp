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
		return GetMaterialStatistics().OutstandingConsumerCount
			+ Private::GetMaterialEditCompileQueue().Num()
			+ GetNumRemainingShaderJobs() + GetNumRemainingMaterialShaderMaps();
	}
	auto FShaderCompilingManager::ProcessAsyncTasks(const FAssetCompileProcessParams& Params)
		-> FAssetCompileProcessResult
	{ return ProcessAsyncResults(Params); }
	auto FShaderCompilingManager::ApplyMaterialResult(FMaterialCompileResult Result, FAssetCompileProcessResult& Processed) -> void
	{
		const auto Resource = Result.Request->Resource;
		auto* Material = Cast<DMaterialInterface>(Resource->Material.Get());
		FObjectCacheContext Context;
		if (IsValid(Material) && Private::FMaterialCompilationLifecycle::Admit(*Material, std::move(Result), &Context))
			Processed.SuccessfullyCompiledAssets.emplace_back(Material);
	}

	auto FShaderCompilingManager::FinishResourceCompilation(const std::shared_ptr<FMaterialCompilationResource>& Resource) -> FAssetCompileProcessResult
	{
		FAssetCompileProcessResult Processed;
		while (const auto Association = Resource->Compilation.lock())
		{
			if (auto* Material = Cast<DMaterialInterface>(Resource->Material.Get()); IsValid(Material))
				Material->PollResourcePreparation(Resource, true);
			FShaderMapPtr Producer;
			{
				std::lock_guard Lock(MaterialMutex);
				const auto Entry = MaterialRequests.find(Resource);
				if (Entry != MaterialRequests.end() && Entry->second.Association == Association) Producer = Entry->second.Request;
			}
			if (Producer) { PrioritizeCompilation(Producer); FinishCompilation(Producer); }
			const auto Consumed = ProcessMaterialResults(
				{.MaximumCompletions = std::numeric_limits<uint32>::max()},
				[&](FMaterialCompileResult Result) { ApplyMaterialResult(std::move(Result), Processed); }, Resource);
			Processed.ProcessedCompletionCount += Consumed;
		}
		return Processed;
	}

	auto FShaderCompilingManager::FinishCompilationForObjects(std::span<DObject* const> Objects) -> FAssetCompileProcessResult
	{
		FAssetCompileProcessResult Aggregate;
		for (auto* Object : Objects)
			if (auto* Material = Cast<DMaterialInterface>(Object); IsValid(Material))
			{
				while (Material->IsDynamicInstance() && IsValid(Material->GetParent())) Material = Material->GetParent();
				if (Material->GetMaterialCompileStatus().HasScheduledEdits())
					Private::FMaterialCompilationLifecycle::RequestCurrent(*Material, false);
				const auto Resources = Material->CompilationState.Resources;
				for (const auto& Resource : Resources)
				{
					if (Resource->Compilation.expired() && (!Resource->bRequested
						|| Material->CompilationState.SubmittedRevision != Material->CompilationState.AuthoredRevision)) continue;
					auto Result = Material->CacheResourceShaders(Resource, EMaterialShaderPrecompileMode::Synchronous);
					Aggregate.ProcessedCompletionCount += Result.ProcessedCompletionCount;
					Aggregate.SuccessfullyCompiledAssets.insert(Aggregate.SuccessfullyCompiledAssets.end(), Result.SuccessfullyCompiledAssets.begin(), Result.SuccessfullyCompiledAssets.end());
				}
			}
		return Aggregate;
	}

	auto FShaderCompilingManager::MarkCompilationAsCanceled(std::span<DObject* const> Objects)
		-> void
	{
		CheckMaterialCompileGameThread();
		for (DObject* Object : Objects)
			if (auto* Material = Cast<DMaterialInterface>(Object); IsValid(Material))
			{
				Private::FMaterialCompilationLifecycle::MarkCanceled(*Material);
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
					if ((Proxy && Proxy->GetRefCount() > 2) || Material->HasResourceRenderReferences())
						RequestMaterialRecompile(*Material, ReloadMode > 1);
				}
		}

		FAssetCompileProcessResult Processed;
		if (!Params.Deadline || std::chrono::steady_clock::now() < *Params.Deadline)
			Private::GetMaterialPreparationQueue().Process(Private::MaterialPreparationMaxChecks, [&](FWeakObjectPtr Owner) {
				auto* Material = Cast<DMaterialInterface>(Owner.Get());
				return IsValid(Material) && Material->PumpResourcePreparations(Params);
			});
		Processed.ProcessedCompletionCount = ProcessMaterialResults(Params, [&](FMaterialCompileResult Result) {
			ApplyMaterialResult(std::move(Result), Processed);
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
			MaterialStatistics.CanceledRequests += OutstandingMaterialConsumers;
			RequestedShaderMaps.clear();
			for (const auto& [Resource, Association] : MaterialRequests)
			{
				Resource->Compilation.reset();
				Resource->Outcome = EMaterialCompileState::Canceled;
			}
			MaterialRequests.clear(); MaterialResults.clear(); OutstandingMaterialConsumers = 0;
		}
		FMaterialShaderMap::ClearCache();
		ClearShaderMapPreparations();
	}
	auto FShaderCompilingManager::IsAcceptingMaterialRequests() const -> bool
	{
		return Accepting && IsTaskSchedulerRunning() && FAssetCompilingManager::Get().IsAcceptingRequests();
	}
}
