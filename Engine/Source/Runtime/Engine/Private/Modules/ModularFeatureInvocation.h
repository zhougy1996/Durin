#pragma once

#include "Modules/ModularFeature.h"

namespace Durin::Private
{
	// Names singleton feature failure diagnostics at domain call sites.
	struct FModularFeatureFailureMessages
	{
		std::string_view Unavailable;
		std::string_view Ambiguous;
		std::string_view VisitorFailed;
	};

	template<CModularFeature TFeature, typename F>
	requires std::same_as<std::invoke_result_t<F, TFeature&>, bool>
	auto InvokeSingleModularFeature(
		F&& Visitor,
		const FModularFeatureFailureMessages& Messages,
		std::string& OutError) -> bool
	{
		const auto Result = FModularFeatureRegistry::Get().InvokeSingle<TFeature>(
			std::forward<F>(Visitor));
		if (Result.Status == EFeatureInvokeStatus::Invoked && Result.Value)
			return *Result.Value;
		if (Result.Status == EFeatureInvokeStatus::Unavailable)
			OutError = Messages.Unavailable;
		else if (Result.Status == EFeatureInvokeStatus::Ambiguous)
			OutError = Messages.Ambiguous;
		else if (Result.Status == EFeatureInvokeStatus::VisitorFailed)
			OutError = Messages.VisitorFailed;
		return false;
	}
}
