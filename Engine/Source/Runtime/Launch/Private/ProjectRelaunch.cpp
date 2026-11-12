#include "ProjectRelaunch.h"
#include "Project/Project.h"
#include "HAL/PlatformProcess.h"

namespace Durin
{
	auto LaunchPendingEditorRelaunch() -> std::expected<void, FProjectError>
	{
        auto Arguments=TakePendingEditorRelaunchArguments();
		if (!Arguments) return {};
		FProcessLaunchOptions Options;
		Options.Arguments.push_back(std::format("--wait-for-process={}", FPlatformProcess::CurrentProcessId()));
		Options.Arguments.insert(Options.Arguments.end(), Arguments->begin(), Arguments->end());
		if (const auto Launched = FPlatformProcess::LaunchProcess(FPlatformProcess::ExecutablePath(), Options); !Launched)
			return std::unexpected(FProjectError{EProjectError::Relaunch, Launched.error().ToString()});
		return {};
	}

}
