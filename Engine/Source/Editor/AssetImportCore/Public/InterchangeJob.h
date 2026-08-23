#pragma once

#include "ImportJob.h"
#include "Interchange.h"

namespace Durin::Asset
{
	using FInterchangeImportCompletion =
		std::function<void(const FInterchangeImportResult&)>;

	struct FInterchangeImportResultState;

	// Releases graph/product preview values before component retirement.
	ASSETIMPORTCORE_API auto ClearInterchangePreviewCache() -> void;

	// Observes one framework-owned interchange job and its immutable terminal
	// inspection/result value.
	class FInterchangeImportHandle
	{
	public:
		FInterchangeImportHandle() = default;
		auto IsValid() const -> bool { return Operation.IsValid() && State != nullptr; }
		explicit operator bool() const { return IsValid(); }
		auto GetOperationHandle() const -> const FImportOperationHandle& { return Operation; }
		ASSETIMPORTCORE_API auto TryGetResult(FInterchangeImportResult& OutResult) const -> bool;

	private:
		FInterchangeImportHandle(FImportOperationHandle InOperation,
			std::shared_ptr<FInterchangeImportResultState> InState)
			: Operation(std::move(InOperation)), State(std::move(InState)) {}

		FImportOperationHandle Operation;
		std::shared_ptr<FInterchangeImportResultState> State;

		friend class FImportService;
	};
}
