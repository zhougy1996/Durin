#pragma once

#include "CoreMinimal.h"

#include "RHIResources.h"

namespace Durin
{
	// Only explicit creation failures may leave the executor usable. Device and
	// invariant failures must not derive from this type.
	class FRHIRecoverableCreationError : public std::exception
	{
	public:
		explicit FRHIRecoverableCreationError(FRHICreationError InError) : Error(InError)
		{ check(Error.HasError()); }
		auto what() const noexcept -> const char* override { return "Recoverable RHI creation failure"; }
		FRHICreationError Error;
	};

	// Executes immediately on the caller; never schedules, acquires a context, or waits.
	// Only explicitly recoverable creation errors become failed results.
	RHI_API auto ExecuteFallibleRHICreationOperation(const std::function<void()>& Operation)
		-> FRHICreationError;

} // namespace Durin
