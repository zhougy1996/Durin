#pragma once

#include "CoreMinimal.h"
#include "DObject/ObjectValidation.h"
#include "Materials/MaterialProgramTypes.h"

namespace Durin
{
	class FMaterialObjectValidationCause final : public IObjectValidationCause
	{
	public:
		std::vector<FMaterialProgramDiagnostic> Diagnostics;
		auto Format() const -> std::string override { return FormatMaterialProgramDiagnostics(Diagnostics, "Invalid material expression graph."); }
	};

	inline auto RejectMaterialObjectGraph(std::string ObjectPath, std::vector<FMaterialProgramDiagnostic> Diagnostics) -> std::expected<void, FObjectValidationError>
	{
		auto Cause = std::make_shared<FMaterialObjectValidationCause>();
		Cause->Diagnostics = std::move(Diagnostics);
		return std::unexpected(FObjectValidationError{.Code = EObjectValidationError::ModuleRejected,
			.ObjectPath = std::move(ObjectPath), .Cause = std::move(Cause)});
	}
}
