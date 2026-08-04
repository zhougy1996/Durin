#pragma once
#include "RHIAPI.h"

namespace Durin
{
	class FDynamicRHI;

	enum class ERHIExecutionMode
	{
		Inline,
		Threaded
	};

	// Resolves DURIN_RHI_EXECUTION semantics. An unset or invalid value keeps
	// the pre-Stage-5 inline default; invalid configured values are diagnosed.
	RHI_API auto ResolveRHIExecutionMode(const char* ConfiguredMode)
		-> ERHIExecutionMode;

	RHI_API auto RHIInit() -> bool;
	RHI_API auto RHIExit() -> void;

	// Takes ownership of Backend. Intended only for isolated initialization
	// failure tests that must not load a platform RHI module.
	RHI_API auto RHIInitWithBackendForTests(
		FDynamicRHI* Backend,
		bool bThreaded,
		bool bForceThreadLaunchFailure = false) -> bool;
}
