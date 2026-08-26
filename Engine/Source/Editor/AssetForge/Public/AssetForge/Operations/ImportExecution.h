#pragma once

#include "AssetForge/Operations/ImportJob.h"
#include "AssetForge/ImportResult.h"

namespace Durin::AssetForge
{
	using FImportCompletion =
		std::function<void(const FImportResult&)>;

	struct FImportResultState;

	// Observes one framework-owned interchange job and its immutable terminal
	// inspection/result value.
	class FImportHandle
	{
	public:
		FImportHandle() = default;
		auto IsValid() const -> bool { return Operation.IsValid() && State != nullptr; }
		explicit operator bool() const { return IsValid(); }
		auto GetOperationHandle() const -> const FImportOperationHandle& { return Operation; }
		ASSETFORGE_API auto TryGetResult(FImportResult& OutResult) const -> bool;

	private:
		FImportHandle(FImportOperationHandle InOperation,
			std::shared_ptr<FImportResultState> InState)
			: Operation(std::move(InOperation)), State(std::move(InState)) {}

		FImportOperationHandle Operation;
		std::shared_ptr<FImportResultState> State;

		friend class FImportService;
	};
}
