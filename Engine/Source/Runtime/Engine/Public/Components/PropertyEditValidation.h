#pragma once
#include <expected>
#include "DObject/ObjectValidation.h"
#include "Materials/MaterialDiagnostic.h"
#include "Spline/SplineMeshDeformer.h"
#include "StaticMesh/StaticMeshMaterialBinding.h"
#include <variant>

namespace Durin
{
	class FEnginePropertyEditCause final : public IObjectValidationCause
	{
	public:
		std::variant<FMaterialError, FSplineMeshValidationError, FStaticMeshMaterialOverrideError> Error;
		auto Format() const -> std::string override
		{
			return std::visit([](const auto& Value) -> std::string {
				using T = std::decay_t<decltype(Value)>;
				if constexpr (std::is_same_v<T, FMaterialError>) return FormatMaterialError(Value);
				else if constexpr (std::is_same_v<T, FSplineMeshValidationError>) return FormatSplineMeshValidationError(Value);
				else return FormatStaticMeshMaterialOverrideError(Value, "mesh component");
			}, Error);
		}
	};
	template<typename TError>
	auto RejectEnginePropertyEdit(const DObject& Object, const FPropertyEditProposal& Proposal, TError Error)
		-> std::expected<void, FObjectValidationError>
	{
		auto Cause = std::make_shared<FEnginePropertyEditCause>();
		Cause->Error = std::move(Error);
		return RejectPropertyEdit(Object, Proposal, EPropertyEditRejection::ModuleRejected, std::move(Cause));
	}
}
