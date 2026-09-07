#pragma once

#include "HAL/Platform.h"
#include <variant>

namespace Durin::Tasks
{
	// Construction failures are separate from the terminal outcome of accepted work.
	enum class ETaskAdmissionErrorCode : uint8
	{
		CapacityExhausted,
		LifetimeClosed,
		GroupClosed,
		InvalidPrerequisite,
		UnsupportedExecutor,
		InvalidPayloadDeclaration,
		UniqueConsumerClaimed,
		DependencyCycle,
		InvalidCallable,
	};

	// Zero means no particular task is associated with the rejection.
	struct FTaskAdmissionError
	{
		ETaskAdmissionErrorCode Code;
		uint64 RelatedTaskId = 0;
	};

	// Has no default success; extracting a move-only value requires an rvalue.
	template<typename T>
	class [[nodiscard]] TTaskAdmission
	{
	public:
		static auto Success(T Value) -> TTaskAdmission
		{
			return TTaskAdmission(std::in_place_index<0>, std::move(Value));
		}
		static auto Failure(FTaskAdmissionError Error) -> TTaskAdmission
		{
			return TTaskAdmission(std::in_place_index<1>, Error);
		}
		auto HasValue() const -> bool { return Value.index() == 0; }
		auto GetError() const -> const FTaskAdmissionError& { return std::get<1>(Value); }
		auto TakeValue() && -> T { return std::get<0>(std::move(Value)); }

	private:
		template<size_t Index, typename U>
		TTaskAdmission(std::in_place_index_t<Index> Tag, U&& InValue)
			: Value(Tag, std::forward<U>(InValue)) {}

		std::variant<T, FTaskAdmissionError> Value;
	};
}
