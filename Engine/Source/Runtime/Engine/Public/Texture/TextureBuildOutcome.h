#pragma once

#include <optional>
#include <string>
#include <utility>

namespace Durin
{
	enum class ETextureBuildFailure
	{
		None,
		InvalidInput,
		BuildFailed,
		Unavailable,
		Ambiguous,
		InvocationFailed,
		InvalidProviderOutput,
		ApplicationFailed
	};
	enum class ETextureBuildStage
	{
		Provider,
		Normalize,
		Recipe,
		Apply
	};

	// Cross-module build status. Diagnostics are presented once by the owning operation.
	struct [[nodiscard]] FTextureBuildOutcome
	{
		ETextureBuildFailure Code = ETextureBuildFailure::BuildFailed;
		ETextureBuildStage Stage = ETextureBuildStage::Provider;
		std::string Diagnostic;
		explicit operator bool() const { return Code == ETextureBuildFailure::None; }
	};

	// A failed operation never publishes a partially built value.
	template<class T> struct [[nodiscard]] TTextureBuildResult
	{
		FTextureBuildOutcome Outcome;
		std::optional<T> Value;
		explicit operator bool() const { return static_cast<bool>(Outcome) && Value.has_value(); }
	};
} // namespace Durin
