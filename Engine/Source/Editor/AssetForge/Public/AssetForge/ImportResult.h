#pragma once

#include "AssetForge/ImportRequest.h"

namespace Durin::AssetForge
{
struct FImportInspection
	{
		bool bCompatible = false;
		std::vector<FImportDiagnostic> Diagnostics;
		std::vector<FImportSourcePreview> Sources;
		std::vector<FImportOutputPreview> Outputs;
		FXxHash128 SourceGraphFingerprint{};
		FXxHash128 BuildGraphFingerprint{};
	};

	struct FImportResult
	{
		FImportOutcome Outcome;
		FImportProvenance Provenance;
		FImportInspection Inspection;
	};
}
