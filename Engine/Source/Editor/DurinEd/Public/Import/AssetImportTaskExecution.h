#pragma once
#include "CoreMinimal.h"
#include "DurinEdAPI.h"

namespace Durin
{
	class DObject;
	class DAssetImportTask;
	// Internal bridge for registered format adapters. This is execution storage,
	// not a second editor import command or a user cancellation protocol.
	class IAssetImportTaskOperation
	{
	public:
		virtual ~IAssetImportTaskOperation() = default;
		virtual auto Tick() -> void = 0;
		virtual auto Cancel() -> void = 0;
		virtual auto IsComplete() const -> bool = 0;
		virtual auto GetObjects() const -> std::span<DObject* const> = 0;
	};

	struct FAssetImportTaskAccess
	{
		DURINED_API static auto Begin(DAssetImportTask& Task) -> bool;
		DURINED_API static auto Attach(DAssetImportTask& Task, std::shared_ptr<IAssetImportTaskOperation> Operation, std::function<void()> Completion) -> void;
		DURINED_API static auto Complete(DAssetImportTask& Task, std::span<DObject* const> Objects) -> void;
		DURINED_API static auto Tick(DAssetImportTask& Task) -> void;
		DURINED_API static auto ReleaseResults(DAssetImportTask& Task) -> void;
		DURINED_API static auto CancelAndDrain(DAssetImportTask& Task) -> void;
	};
}
