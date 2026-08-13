#include "ProcessCrashServices.h"

#include "WindowsProcessCrashHandler.h"

namespace Durin
{
	auto InstallProcessCrashHandler() -> bool
	{
		return InstallWindowsProcessCrashHandler();
	}

	auto PublishProcessCrashRoot(
		std::string_view SavedDirectory,
		bool bExplicitDiagnosticOverride) -> bool
	{
		return PublishWindowsProcessCrashRoot(
			SavedDirectory, bExplicitDiagnosticOverride);
	}

	auto UninstallProcessCrashHandler() -> void
	{
		UninstallWindowsProcessCrashHandler();
	}

	auto ConfigureProcessCrashTestOptions(
		bool bDisableDump,
		bool bForceCollision,
		bool bFaultCrashWriter) -> void
	{
		ConfigureWindowsProcessCrashTestOptions(
			bDisableDump, bForceCollision, bFaultCrashWriter);
	}

	auto RunProcessCrashFixture(std::string_view Fixture) -> bool
	{
		return RunWindowsProcessCrashFixture(Fixture);
	}
}
