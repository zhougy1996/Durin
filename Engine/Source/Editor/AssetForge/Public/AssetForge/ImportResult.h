#pragma once

#include "AssetForge/ImportRequest.h"

namespace Durin::AssetForge
{
struct FImportInspection
	{
		bool bCompatible = false;
		std::vector<FImportDiagnostic> Diagnostics;
		std::vector<FImportSourceSummary> Sources;
		std::vector<FImportOutputSummary> Outputs;
		FXxHash128 SourceGraphFingerprint{};
		FXxHash128 BuildGraphFingerprint{};
	};

	enum class EImportPersistenceState : uint8
	{
		NotRequested,
		Succeeded,
		Failed
	};

	struct FImportPersistenceResult
	{
		EImportPersistenceState State = EImportPersistenceState::NotRequested;
		std::string Diagnostic;

		auto operator==(const FImportPersistenceResult&) const -> bool = default;
	};

	struct FImportResult
	{
		FImportOutcome Outcome;
		FImportProvenance Provenance;
		FImportInspection Inspection;
		FImportPersistenceResult Persistence;
	};
}
