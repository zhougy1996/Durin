#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	// Required platform service for process crash handling and qualification fixtures.
	auto InstallProcessCrashHandler() -> bool;
	auto PublishProcessCrashRoot(
		std::string_view SavedDirectory,
		bool bExplicitDiagnosticOverride = false) -> bool;
	auto UninstallProcessCrashHandler() -> void;
	auto ConfigureProcessCrashTestOptions(
		bool bDisableDump,
		bool bForceCollision,
		bool bFaultCrashWriter) -> void;
	auto RunProcessCrashFixture(std::string_view Fixture) -> bool;
}
