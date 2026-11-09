#pragma once

#include "CoreMinimal.h"
#include "Asset/PackageReload.h"
#include "Materials/MaterialCompileLifecycle.h"

namespace Durin
{
	// Owns diagnostic values, never references to a rejected candidate graph.
	struct FMaterialPackageReloadDiagnosticDetails final : FPackageReloadDiagnosticDetails
	{
		std::optional<FMaterialCompileStatus> CompileStatus;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;
		ENGINE_API auto GetSummary() const -> std::string override;
	};
}
