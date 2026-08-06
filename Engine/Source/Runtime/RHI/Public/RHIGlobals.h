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

	// Resolves DURIN_RHI_EXECUTION semantics. Threaded execution is the normal
	// path; "inline" remains an explicit diagnostic override. Invalid configured
	// values are diagnosed and use the normal threaded path.
	RHI_API auto ResolveRHIExecutionMode(const char* ConfiguredMode)
		-> ERHIExecutionMode;

	RHI_API auto RHIInit() -> bool;
	RHI_API auto RHIExit() -> void;
	// Retains the owned cause from the most recent failed initialization attempt.
	// A later successful attempt clears it.
	RHI_API auto GetLastRHIInitializationDiagnostic() -> std::string_view;

	// Takes ownership of Backend. Intended only for isolated initialization
	// failure tests that must not load a platform RHI module.
	RHI_API auto RHIInitWithBackendForTests(
		FDynamicRHI* Backend,
		bool bThreaded,
		bool bForceThreadLaunchFailure = false) -> bool;
}
