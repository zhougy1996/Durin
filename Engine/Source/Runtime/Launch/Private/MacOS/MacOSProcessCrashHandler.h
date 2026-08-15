#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	auto InstallMacOSProcessCrashHandler() -> bool;
	auto PublishMacOSProcessCrashRoot(
		std::string_view SavedDirectory,
		bool bExplicitDiagnosticOverride = false) -> bool;
	auto UninstallMacOSProcessCrashHandler() -> void;
	auto ConfigureMacOSProcessCrashTestOptions(
		bool bDisableDump,
		bool bForceCollision,
		bool bFaultCrashWriter) -> void;
	auto RunMacOSProcessCrashFixture(std::string_view Fixture) -> bool;
}
