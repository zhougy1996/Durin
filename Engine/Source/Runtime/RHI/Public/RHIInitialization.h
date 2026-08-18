#pragma once

#include "RHIAPI.h"

namespace Durin
{
	// Describes a live non-owning platform target used for presentation admission.
	struct FRHIPresentationTarget
	{
		void* NativeWindowHandle = nullptr;

		auto IsValid() const -> bool
		{
			return NativeWindowHandle != nullptr;
		}
	};

	// Carries exactly one explicit headless or presentation-aware startup mode.
	class FRHIInitializationContext
	{
	public:
		static auto Headless() -> FRHIInitializationContext
		{
			return FRHIInitializationContext();
		}

		static auto Presentation(FRHIPresentationTarget Target)
			-> FRHIInitializationContext
		{
			require(Target.IsValid());
			FRHIInitializationContext Context;
			Context.PresentationTarget = Target;
			return Context;
		}

		auto IsHeadless() const -> bool
		{
			return !PresentationTarget.has_value();
		}

		auto GetPresentationTarget() const
			-> const std::optional<FRHIPresentationTarget>&
		{
			return PresentationTarget;
		}

	private:
		std::optional<FRHIPresentationTarget> PresentationTarget;
	};
}
