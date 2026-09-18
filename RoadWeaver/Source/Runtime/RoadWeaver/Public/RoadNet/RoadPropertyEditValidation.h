#pragma once
#include "RoadNet/RoadSurface.h"
#include <variant>

namespace Durin::RoadNet
{
	class FRoadPropertyEditCause final : public IObjectValidationCause
	{
	public:
		std::variant<FRoadDefinitionError, FRoadPlacementError> Error;
		auto Format() const -> std::string override
		{
			return std::visit([](const auto& Value) -> std::string {
				if constexpr (std::is_same_v<std::decay_t<decltype(Value)>, FRoadDefinitionError>)
					return FormatRoadDefinitionError(Value);
				else return FormatRoadPlacementError(Value);
			}, Error);
		}
	};
	template<typename TError>
	auto RejectRoadPropertyEdit(const DObject& Object, const FPropertyEditProposal& Proposal, TError Error)
		-> FObjectValidationResult
	{
		auto Cause = std::make_shared<FRoadPropertyEditCause>();
		Cause->Error = std::move(Error);
		return RejectPropertyEdit(Object, Proposal, EPropertyEditRejection::ModuleRejected, std::move(Cause));
	}
}
