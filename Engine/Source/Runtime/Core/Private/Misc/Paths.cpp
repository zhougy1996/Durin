#include "Misc/Paths.h"

#include "HAL/PlatformProcess.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace PathUtilities
	{
		static std::vector<FMountPoint> MountPoints;

		auto GetRegisteredMountPoints() -> const std::vector<FMountPoint>&
		{
			return MountPoints;
		}

		static auto RegisterMountPointWithoutSorting(std::string_view VirtualRoot, std::string_view PhysicalPath) -> void
		{
			const auto FoundIt = std::ranges::find_if(MountPoints, [VirtualRoot](const FMountPoint& MountPoint) {
				return MountPoint.VirtualRoot == VirtualRoot;
			});

			if (FoundIt != MountPoints.end())
			{
				FoundIt->PhysicalPath = PhysicalPath;
			}
			else
			{
				MountPoints.push_back({std::string(VirtualRoot), std::string(PhysicalPath)});
				DURIN_DEBUG("Mount point: {} -> {}", VirtualRoot, PhysicalPath);
			}
		}

		auto RegisterMountPoint(std::string_view VirtualRoot, std::string_view PhysicalPath) -> void
		{
			checkf(IsInGameThread(), "AddMountPoint must be called from the game thread.");
			RegisterMountPointWithoutSorting(VirtualRoot, PhysicalPath);

			// Sort the mount points by the length of their virtual root in descending order, so that we can match the longest virtual root first when resolving paths.
			std::ranges::sort(MountPoints, [](const FMountPoint& A, const FMountPoint& B) {
				return A.VirtualRoot.length() > B.VirtualRoot.length();
			});
		}

		auto InitDefaultMountPoints() -> void
		{
			checkf(IsInGameThread(), "InitDefaultMountPoints must be called from the game thread.");
			RegisterMountPointWithoutSorting("/Engine/", FPaths::EngineDir() + "Content/");
		}
	} // namespace PathUtilities

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

	auto FPaths::ProjectDir() -> std::string
	{
		// TODO: Implement this function to return the actual project directory.
		return EngineDir();
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

	auto FPaths::Resolve(std::string_view VirtualPath) -> std::string
	{
		std::string NormalizedVirtualPath = std::filesystem::path{VirtualPath}.lexically_normal().generic_string();
		for (const auto& MountPoint : PathUtilities::MountPoints)
		{
			if (NormalizedVirtualPath.starts_with(MountPoint.VirtualRoot))
			{
				std::string RelativePath = std::string(NormalizedVirtualPath.substr(MountPoint.VirtualRoot.size()));
				return MountPoint.PhysicalPath + RelativePath;
			}
		}

		return NormalizedVirtualPath;
	}

} // namespace Doge