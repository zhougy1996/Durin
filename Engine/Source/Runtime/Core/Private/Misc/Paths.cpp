#include "Misc/Paths.h"

#include "Misc/ConfigCacheJson.h"
#include "HAL/PlatformProcess.h"

namespace Doge
{
	auto FPaths::LaunchDir() -> std::string
	{
		static std::string CachedLaunchDir = []() -> std::string {
			std::string ExePath = FPlatformProcess::ExecutablePath();
			std::string LaunchDir = std::filesystem::path{ExePath}.parent_path().generic_string() + "/";
			return LaunchDir;
		}();
		return CachedLaunchDir;
	}

	auto FPaths::RootDir() -> std::string
	{
		static std::string CachedRootDir = []() -> std::string {
			const std::string LaunchDir = FPaths::LaunchDir();
			const std::filesystem::path RootDir = std::filesystem::path{LaunchDir}.parent_path().parent_path().parent_path().parent_path().parent_path().parent_path();
			return RootDir.generic_string() + "/";
		}();
		return CachedRootDir;
	}

	auto FPaths::EngineDir() -> std::string
	{
		static std::string CachedEngineDir = []() -> std::string {
			const std::string RootDir = FPaths::RootDir();
			return RootDir + "Engine/";
		}();
		return CachedEngineDir;
	}

	auto FPaths::EngineContentDir() -> std::string
	{
		static std::string EngineContentDir = []() -> std::string {
			return std::filesystem::path{EngineDir()}.append("Content/").generic_string();
		}();
		return EngineContentDir;
	}

	auto FPaths::EngineBinariesDir() -> std::string
	{
		return EngineDir() + "Binaries/";
	}

	auto FPaths::EngineThirdPartyRuntimeBinariesDir() -> std::string
	{
		return EngineBinariesDir() + std::format("ThirdParty/{}/{}/", DOGE_BUILD_PLATFORM_STRING, DOGE_BUILD_TYPE_STRING);
	}
} // namespace Doge