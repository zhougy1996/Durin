#include "Misc/Paths.h"

#include "HAL/PlatformProcess.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		std::filesystem::path ActiveProjectFile;

		auto IsEngineDirCandidate(const std::filesystem::path& Candidate) -> bool
		{
			return std::filesystem::is_directory(Candidate)
				&& std::filesystem::is_directory(Candidate / "Content")
				&& std::filesystem::is_directory(Candidate / "Binaries");
		}

		auto FindEngineDirFromLaunchDir() -> std::filesystem::path
		{
			const std::filesystem::path LaunchPath(FPaths::LaunchDir());

			for (auto Current = LaunchPath; !Current.empty(); Current = Current.parent_path())
			{
				const std::filesystem::path EngineDir = Current / "Engine";
				if (IsEngineDirCandidate(EngineDir))
				{
					return EngineDir;
				}

				if (Current == Current.parent_path())
				{
					break;
				}
			}

			checkf(false, "Failed to locate Engine directory from launch directory.");
			return {};
		}

		auto FindOutputRootDirFromLaunchDir() -> std::filesystem::path
		{
			const std::filesystem::path LaunchPath = std::filesystem::path(FPaths::LaunchDir()).lexically_normal();
			const std::filesystem::path OutputDir = LaunchPath.filename().empty()
				? LaunchPath.parent_path()
				: LaunchPath;

			for (auto Current = OutputDir; !Current.empty(); Current = Current.parent_path())
			{
				if (Current.filename() == "Tests")
				{
					return Current.parent_path();
				}

				if (Current == Current.parent_path())
				{
					break;
				}
			}

			const std::filesystem::path RuntimeDir = OutputDir.parent_path();

			checkf(RuntimeDir.filename() == "Runtime",
				"Expected launch directory to be under a Runtime/<Profile> or Tests/<Profile>/<Target>/Bin layout. LaunchDir={}",
				FPaths::LaunchDir());

			return RuntimeDir.parent_path();
		}
	}

	auto FPaths::SetProjectFile(std::string_view ProjectFile, std::string* OutError) -> bool
	{
		if (ProjectFile.empty())
		{
			ActiveProjectFile.clear();
			return true;
		}

		const std::filesystem::path Candidate = std::filesystem::absolute(ProjectFile).lexically_normal();
		if (Candidate.extension() != ".dproject" || !std::filesystem::is_regular_file(Candidate))
		{
			if (OutError) *OutError = std::format("Project file does not exist or is not a .dproject file: {}", Candidate.generic_string());
			return false;
		}
		ActiveProjectFile = Candidate;
		return true;
	}

	auto FPaths::ProjectFile() -> std::string
	{
		return ActiveProjectFile.generic_string();
	}

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

			if (!FPaths::ProjectFile().empty())
			{
				const std::filesystem::path ContentDir = std::filesystem::path(FPaths::ProjectFile()).parent_path() / "Content";
				RegisterMountPointWithoutSorting(ProjectContentMountRoot, ContentDir.generic_string() + "/");
			}

			std::ranges::sort(MountPoints, [](const FMountPoint& A, const FMountPoint& B) {
				return A.VirtualRoot.length() > B.VirtualRoot.length();
			});
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
			std::filesystem::path EnginePath{FPaths::EngineDir()};
			if (EnginePath.filename().empty()) EnginePath = EnginePath.parent_path();
			const std::filesystem::path RootDir = EnginePath.parent_path();
			return RootDir.generic_string() + "/";
		}();
		return CachedRootDir;
	}

	auto FPaths::EngineDir() -> std::string
	{
		static std::string CachedEngineDir = []() -> std::string {
			return FindEngineDirFromLaunchDir().generic_string() + "/";
		}();
		return CachedEngineDir;
	}

	auto FPaths::ProjectDir() -> std::string
	{
		if (!ActiveProjectFile.empty()) return ActiveProjectFile.parent_path().generic_string() + "/";
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
		static std::string CachedThirdPartyDir = []() -> std::string {
			const std::filesystem::path ConfigDir = FindOutputRootDirFromLaunchDir();
			return (ConfigDir / "ThirdParty").generic_string() + "/";
		}();
		return CachedThirdPartyDir;
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

} // namespace Durin
