#include "Asset/CookedMeshLoadManagerPrivate.h"

#include "StaticMesh/StaticMeshLoadUpdate.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		std::shared_ptr<FCookedMeshLoadManager> GManager;
	}

	FCookedMeshLoadManager::FCookedMeshLoadManager(FCookedMeshLoadManagerConfig InConfig)
		: Config(InConfig) {}

	FCookedMeshLoadManager::~FCookedMeshLoadManager()
	{
		Shutdown();
	}

	auto FCookedMeshLoadManager::Initialize() -> bool
	{
		CheckGameThread();
		if (bAccepting) return true;
		if (!IsTaskSchedulerRunning() || Config.MaxConcurrentRequests == 0
			|| Config.MaxEstimatedBytes == 0 || Config.MaxPendingCompletionBytes == 0
			|| Config.MaxIoPollsPerPump == 0 || Config.MaxCompletionsPerPump == 0) return false;
		Attribution = RegisterTaskAttribution("Engine", "CookedMeshLoad");
		bAccepting = true;
		return true;
	}

	auto FCookedMeshLoadManager::Submit(std::shared_ptr<FStaticMeshLoadUpdate> Update) -> bool
	{
		CheckGameThread();
		check(Update && !Update->IsCompleted());
		if (!bAccepting)
			Update->Error = {.Code = ECookedMeshLoadError::Unavailable,
				.Diagnostic = "Cooked mesh loading is not accepting updates."};
		if (Update->Error.Code != ECookedMeshLoadError::None)
		{
			Update->Tick();
			return false;
		}
		Pending.push_back(std::move(Update));
		++Diagnostics.AcceptedCount;
		return true;
	}

	auto FCookedMeshLoadManager::RecordCompletion(const FStaticMeshLoadUpdate& Update) -> void
	{
		if (Update.IsCancelled()) ++Diagnostics.CancelledCount;
		else if (Update.IsSuccessfullyFinished()) ++Diagnostics.SucceededCount;
		else ++Diagnostics.FailedCount;
	}

	auto FCookedMeshLoadManager::Pump() -> uint32
	{
		CheckGameThread();
		// Finished task bodies own their results until GameThread publication.
		// Apply the aggregate completion budget before consuming any of them.
		uint64 CompletionBytes = 0;
		for (const auto& Update : Active)
		{
			if (Update->Phase == FStaticMeshLoadUpdate::EPhase::Working && Update->Task.IsCompleted())
				Update->Phase = FStaticMeshLoadUpdate::EPhase::Ready;
			if (Update->Phase != FStaticMeshLoadUpdate::EPhase::Ready
				|| Update->Task.GetState() != ETaskState::Succeeded || Update->IsCancelled()
				|| Update->Error.Code != ECookedMeshLoadError::None) continue;
			const uint64 Bytes = Update->RetainedBytes;
			if (Bytes == 0 || Bytes > Config.MaxPendingCompletionBytes
				|| CompletionBytes > Config.MaxPendingCompletionBytes - Bytes)
			{
				Update->Product = {};
				Update->RetainedBytes = 0;
				Update->Error = {.Code = ECookedMeshLoadError::CompletionBudget,
					.Diagnostic = std::format("Cooked mesh result needs {} bytes with {} reserved against a {} byte completion limit.",
						Bytes, CompletionBytes, Config.MaxPendingCompletionBytes)};
			}
			else CompletionBytes += Bytes;
		}

		uint32 Completed = 0;
		uint32 Polled = 0;
		for (size_t Index = 0; Index < Active.size() && Completed < Config.MaxCompletionsPerPump;)
		{
			const auto Update = Active[Index];
			// A worker that finishes after the budget pass waits for the next pump.
			if (Update->Phase == FStaticMeshLoadUpdate::EPhase::Working) { ++Index; continue; }
			if (Update->Phase == FStaticMeshLoadUpdate::EPhase::Reading
				&& Polled++ >= Config.MaxIoPollsPerPump) { ++Index; continue; }
			Update->Tick();
			if (!Update->IsCompleted()) { ++Index; continue; }
			check(InFlightEstimatedBytes >= Update->EstimatedBytes);
			InFlightEstimatedBytes -= Update->EstimatedBytes;
			RecordCompletion(*Update);
			Active.erase(Active.begin() + Index);
			++Completed;
		}

		for (size_t Index = 0; Index < Pending.size();)
		{
			const auto Update = Pending[Index];
			if (Update->IsCancelled()) Update->Tick();
			if (Update->IsCompleted())
			{
				RecordCompletion(*Update);
				Pending.erase(Pending.begin() + Index);
				continue;
			}
			if (!bAccepting || Active.size() >= Config.MaxConcurrentRequests
				|| (!Active.empty() && (Update->EstimatedBytes > Config.MaxEstimatedBytes
					|| InFlightEstimatedBytes > Config.MaxEstimatedBytes - Update->EstimatedBytes)))
			{
				++Index;
				continue;
			}
			Update->Start(Attribution);
			InFlightEstimatedBytes += Update->EstimatedBytes;
			Active.push_back(Update);
			Pending.erase(Pending.begin() + Index);
			Diagnostics.PeakInFlightCount = std::max<uint32>(
				Diagnostics.PeakInFlightCount, static_cast<uint32>(Active.size()));
			Diagnostics.PeakInFlightEstimatedBytes = std::max(
				Diagnostics.PeakInFlightEstimatedBytes, InFlightEstimatedBytes);
		}
		return Completed;
	}

	auto FCookedMeshLoadManager::Shutdown() -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		bAccepting = false;
		for (const auto& Update : Pending) Update->Abort();
		for (const auto& Update : Active) Update->Abort();
		for (const auto& Update : Pending) { Update->Finish(); RecordCompletion(*Update); }
		for (const auto& Update : Active) { Update->Finish(); RecordCompletion(*Update); }
		Pending.clear();
		Active.clear();
		InFlightEstimatedBytes = 0;
	}

	auto FCookedMeshLoadManager::GetDiagnostics() const -> FCookedMeshLoadDiagnostics
	{
		CheckGameThread();
		auto Result = Diagnostics;
		Result.InFlightCount = static_cast<uint32>(Active.size());
		Result.PendingRequestCount = static_cast<uint32>(Pending.size());
		Result.InFlightEstimatedBytes = InFlightEstimatedBytes;
		for (const auto& Update : Active)
			if (Update->Phase == FStaticMeshLoadUpdate::EPhase::Ready
				|| (Update->Phase == FStaticMeshLoadUpdate::EPhase::Working && Update->Task.IsCompleted()))
			{
				++Result.PendingCompletionCount;
				Result.PendingCompletionBytes += Update->RetainedBytes;
			}
		return Result;
	}

	auto InitializeCookedMeshLoadManager() -> bool
	{
		CheckGameThread();
		if (!GManager) GManager = std::make_shared<FCookedMeshLoadManager>();
		return GManager->Initialize();
	}

	auto PumpCookedMeshLoadManager() -> uint32
	{
		CheckGameThread();
		const auto Manager = GManager;
		return Manager ? Manager->Pump() : 0;
	}

	auto ShutdownCookedMeshLoadManager() -> void
	{
		CheckGameThread();
		auto Manager = std::move(GManager);
		if (Manager) Manager->Shutdown();
	}

	auto GetCookedMeshLoadManager() -> FCookedMeshLoadManager*
	{
		CheckGameThread();
		return GManager.get();
	}
}
