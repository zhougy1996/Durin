#include "ProcessCrashServices.h"

#include "MacOSProcessCrashHandler.h"

namespace Durin
{
	auto InstallProcessCrashHandler() -> bool
	{
		return InstallMacOSProcessCrashHandler();
	}

	auto PublishProcessCrashRoot(
		std::string_view SavedDirectory,
		bool bExplicitDiagnosticOverride) -> bool
	{
		return PublishMacOSProcessCrashRoot(
			SavedDirectory, bExplicitDiagnosticOverride);
	}

	auto UninstallProcessCrashHandler() -> void
	{
		UninstallMacOSProcessCrashHandler();
	}

	auto ConfigureProcessCrashTestOptions(
		bool bDisableDump,
		bool bForceCollision,
		bool bFaultCrashWriter) -> void
	{
		ConfigureMacOSProcessCrashTestOptions(
			bDisableDump, bForceCollision, bFaultCrashWriter);
	}

	auto RunProcessCrashFixture(std::string_view Fixture) -> bool
	{
		return RunMacOSProcessCrashFixture(Fixture);
	}
}
