#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	// Owns the process-wide Windows native-crash filter and local artifact root.
	auto InstallWindowsProcessCrashHandler() -> bool;
	auto PublishWindowsProcessCrashRoot(std::string_view SavedDirectory) -> bool;
	auto UninstallWindowsProcessCrashHandler() -> void;
	auto ConfigureWindowsProcessCrashTestOptions(bool bDisableDump) -> void;

	// Invokes non-Shipping isolated fault fixtures used by native qualification.
	auto RunWindowsProcessCrashFixture(std::string_view Fixture) -> bool;
}
