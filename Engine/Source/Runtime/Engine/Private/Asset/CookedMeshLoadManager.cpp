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
			|| Config.MaxCompletionsPerPump == 0) return false;
		Attribution = RegisterTaskAttribution("Engine", "CookedMeshLoad");
		bAccepting = true;
		return true;
	}

	auto FCookedMeshLoadManager::Submit(std::shared_ptr<FStaticMeshLoadUpdate> Update) -> bool
	{
		CheckGameThread();
		check(Update && !Update->IsCompleted());
		if (!bAccepting)
		{
			Update->Reject();
			return false;
		}
		Update->Tick();
		if (Update->IsCompleted()) return false;
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
		uint32 Completed = 0;
		for (size_t Index = 0; Index < Active.size() && Completed < Config.MaxCompletionsPerPump;)
		{
			const auto Update = Active[Index];
			Update->Tick();
			if (!Update->IsCompleted()) { ++Index; continue; }
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
			if (!bAccepting || Active.size() >= Config.MaxConcurrentRequests)
			{
				++Index;
				continue;
			}
			Update->Start(Attribution);
			Active.push_back(Update);
			Pending.erase(Pending.begin() + Index);
			Diagnostics.PeakInFlightCount = std::max<uint32>(
				Diagnostics.PeakInFlightCount, static_cast<uint32>(Active.size()));
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
	}

	auto FCookedMeshLoadManager::GetDiagnostics() const -> FCookedMeshLoadDiagnostics
	{
		CheckGameThread();
		auto Result = Diagnostics;
		Result.InFlightCount = static_cast<uint32>(Active.size());
		Result.PendingRequestCount = static_cast<uint32>(Pending.size());
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
