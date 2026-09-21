#pragma once
#include <expected>
#include "DObject/ObjectValidation.h"
#include "Materials/MaterialProgramTypes.h"

namespace Durin
{
	class FMaterialObjectValidationCause final : public IObjectValidationCause
	{
	public:
		FMaterialError Error;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;
		auto Format() const -> std::string override { return FormatMaterialError(Error); }
	};

	inline auto RejectMaterialObjectGraph(std::string ObjectPath, FMaterialError Error,
		std::vector<FMaterialProgramDiagnostic> Diagnostics = {}) -> std::expected<void, FObjectValidationError>
	{
		auto Cause = std::make_shared<FMaterialObjectValidationCause>();
		Cause->Error = std::move(Error);
		Cause->Diagnostics = std::move(Diagnostics);
		return std::unexpected(FObjectValidationError{.Code = EObjectValidationError::ModuleRejected,
			.ObjectPath = std::move(ObjectPath), .Cause = std::move(Cause)});
	}
}
