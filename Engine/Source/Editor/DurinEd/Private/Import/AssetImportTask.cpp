#include "AssetImportTask.h"
#include "Factories/Factory.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	DAssetImportTask::DAssetImportTask(const FObjectInitializer& Initializer) : Super(Initializer) {}

	auto DAssetImportTask::GetObjects() const -> const std::vector<DObject*>&
	{
		check(IsInGameThread());
		// A synchronous factory cannot wait on its own invocation from a callback.
		checkf(!bRunning, "Cannot wait for an import task from inside its synchronous factory callback.");
		ObjectView.clear();
		for (const auto& Object : Results) if (Object.IsValid()) ObjectView.push_back(Object.Get());
		return ObjectView;
	}

	auto DAssetImportTask::IsAsyncImportComplete() const -> bool
	{
		check(IsInGameThread());
		return !bRunning;
	}
}
