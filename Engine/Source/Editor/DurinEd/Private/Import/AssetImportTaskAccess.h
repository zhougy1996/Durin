#pragma once
#include "CoreMinimal.h"
#include "DurinEdAPI.h"
#include "Import/AssetImportTaskOperation.h"

namespace Durin
{
	class DAssetImportTask;
	// AssetTools alone submits and advances tasks. DurinEd owns all result transitions.
	struct FAssetImportTaskAccess
	{
		DURINED_API static auto Begin(DAssetImportTask& Task) -> bool;
		DURINED_API static auto Attach(DAssetImportTask& Task, std::shared_ptr<IAssetImportTaskOperation> Operation, std::function<void()> Completion) -> void;
		DURINED_API static auto CompleteSynchronous(DAssetImportTask& Task, std::span<DObject* const> Objects) -> void;
		DURINED_API static auto Tick(DAssetImportTask& Task) -> void;
		DURINED_API static auto CancelAndDrain(DAssetImportTask& Task) -> void;
	private:
		static auto Complete(DAssetImportTask& Task, std::span<DObject* const> Objects) -> void;
	};
}
