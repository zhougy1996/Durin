#pragma once

#include <expected>
#include <string>
#include <utility>

namespace Durin
{
	enum class ETextureBuildFailure
	{
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

	// Build failure context. Diagnostics are presented once by the owning operation.
	struct [[nodiscard]] FTextureBuildError
	{
		ETextureBuildFailure Code = ETextureBuildFailure::BuildFailed;
		ETextureBuildStage Stage = ETextureBuildStage::Provider;
		std::string Diagnostic;
	};

} // namespace Durin
