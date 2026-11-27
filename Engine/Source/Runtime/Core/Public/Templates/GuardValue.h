#pragma once

#include "Misc/CoreStd.h"

namespace Durin
{
	// Temporarily assigns a value and restores the previous value on scope exit.
	template<typename T>
	class TGuardValue
	{
	public:
		TGuardValue(T& Target, T Value)
			: Target(Target), Previous(std::exchange(Target, std::move(Value))) {}
		~TGuardValue() { Target = std::move(Previous); }
		TGuardValue(const TGuardValue&) = delete;
		auto operator=(const TGuardValue&) -> TGuardValue& = delete;

	private:
		T& Target;
		T Previous;
	};
}
