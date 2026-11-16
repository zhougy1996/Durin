#include "AssetImportTask.h"
#include "Factories/Factory.h"
#include "Import/AssetImportTaskExecution.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	DAssetImportTask::DAssetImportTask(const FObjectInitializer& Initializer) : Super(Initializer) {}

	auto FAssetImportTaskAccess::Begin(DAssetImportTask& Task) -> bool
	{
		check(IsInGameThread());
		if (Task.bRunning) return false;
		Task.Results.clear(); Task.ObjectView.clear(); Task.ImportedObjectPaths.clear();
		Task.bRunning = true;
		return true;
	}
	auto FAssetImportTaskAccess::Attach(DAssetImportTask& Task, std::shared_ptr<IAssetImportTaskOperation> Operation, std::function<void()> Completion) -> void
	{ check(Task.bRunning && !Task.Operation); Task.Operation = std::move(Operation); Task.Completion = std::move(Completion); }
	auto FAssetImportTaskAccess::Complete(DAssetImportTask& Task, std::span<DObject* const> Objects) -> void
	{
		for (auto* Object : Objects) if (IsValid(Object))
		{
			Task.Results.emplace_back(Object);
			Task.ImportedObjectPaths.push_back(Object->GetObjectPath());
		}
		Task.bRunning = false;
		Task.Operation.reset();
		const auto Completion = std::exchange(Task.Completion, {});
		if (Completion) Completion();
	}
	auto FAssetImportTaskAccess::Tick(DAssetImportTask& Task) -> void
	{
		check(IsInGameThread());
		if (!Task.bRunning || !Task.Operation || Task.bAdvancing) return;
		Task.bAdvancing = true;
		struct FGuard { bool& Flag; ~FGuard() { Flag = false; } } Guard{Task.bAdvancing};
		const auto Operation = Task.Operation;
		Operation->Tick();
		if (Operation->IsComplete()) Complete(Task, Operation->GetObjects());
	}
	auto FAssetImportTaskAccess::ReleaseResults(DAssetImportTask& Task) -> void
	{
		check(IsInGameThread()); check(!Task.bRunning);
		Task.Results.clear(); Task.ObjectView.clear(); Task.ImportedObjectPaths.clear();
	}
	auto FAssetImportTaskAccess::CancelAndDrain(DAssetImportTask& Task) -> void
	{
		checkf(!Task.bAdvancing, "Cannot retire an import provider from its own execution callback.");
		if (Task.Operation) Task.Operation->Cancel();
		(void)Task.GetObjects();
	}
	auto DAssetImportTask::GetObjects() const -> const std::vector<DObject*>&
	{
		check(IsInGameThread());
		checkf(!bAdvancing && (!bRunning || Operation), "Cannot wait for an import task from its synchronous callback.");
		auto& Mutable = const_cast<DAssetImportTask&>(*this);
		while (bRunning)
		{
			FAssetImportTaskAccess::Tick(Mutable);
			if (bRunning) std::this_thread::yield();
		}
		ObjectView.clear();
		for (const auto& Object : Results) if (Object.IsValid()) ObjectView.push_back(Object.Get());
		return ObjectView;
	}
	auto DAssetImportTask::IsAsyncImportComplete() const -> bool
	{ check(IsInGameThread()); return !bRunning; }
}
