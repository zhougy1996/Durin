#pragma once
#include "Delegates/Delegate.h"
#include "RHIAPI.h"
#include "RHIInitialization.h"

namespace Durin
{
	class FDynamicRHI;
	DECLARE_MULTICAST_DELEGATE(FRHIReleaseResourcesDelegate)

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

	RHI_API auto RHIInit(FRHIInitializationContext Context) -> bool;
	RHI_API auto RHIExit() -> void;
	// Broadcast before the command-list flush and backend shutdown so upper
	// layers can drop device-backed references while the RHI is still valid.
	RHI_API auto GetRHIReleaseResourcesDelegate()
		-> FRHIReleaseResourcesDelegate&;
	// Retains the owned cause from the most recent failed initialization attempt.
	// A later successful attempt clears it.
	RHI_API auto GetLastRHIInitializationDiagnostic() -> std::string_view;

	// Takes ownership of Backend. Intended only for isolated initialization
	// failure tests that must not load a platform RHI module.
	RHI_API auto RHIInitWithBackendForTests(
		FDynamicRHI* Backend,
		bool bThreaded,
		bool bForceThreadLaunchFailure,
		FRHIInitializationContext Context) -> bool;
}
