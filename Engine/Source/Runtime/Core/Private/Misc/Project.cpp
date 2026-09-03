#include "Misc/Project.h"

#include "CoreGlobals.h"
#include "HAL/PlatformProcess.h"
#include "Json/Json.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/ProjectHistory.h"
#include "Misc/StringHelper.h"

#ifdef _WIN32
	#include "Windows/WindowsPlatform.h"
#elif defined(__APPLE__)
	#include <fcntl.h>
	#include <sys/file.h>
	#include <unistd.h>
#endif

namespace Durin
{
	namespace
	{
		std::optional<FProjectInfo> GCurrentProject;
		std::optional<std::string> GPendingEditorRelaunchArguments;
#ifdef _WIN32
		HANDLE GProjectEditMutex = nullptr;
#elif defined(__APPLE__)
		int GProjectEditFile = -1;
		pid_t GProjectEditOwnerProcess = 0;
#endif

		auto Normalize(const std::filesystem::path& Path) -> std::string
		{
			std::error_code Error;
			const std::filesystem::path Absolute = std::filesystem::absolute(Path, Error).lexically_normal();
			return (Error ? Path.lexically_normal() : Absolute).generic_string();
		}

		auto ReadModuleArray(
			FJsonNodeView Array,
			std::string_view FieldName,
			std::vector<std::string>& OutModules,
			std::unordered_set<std::string>& SeenModules,
			std::string* OutError
		) -> bool
		{
			if (!Array.IsArray())
			{
				if (OutError) *OutError = std::format(
					"Project descriptor field '{}' must be an array of module names.", FieldName);
				return false;
			}
			for (size_t Index = 0; Index < Array.Num(); ++Index)
			{
				const FJsonNodeView Entry = Array.GetView(Index);
				if (!Entry.IsString() || Entry.GetString().empty())
				{
					if (OutError) *OutError = std::format(
						"Project descriptor field '{}[{}]' must be a non-empty module name.",
						FieldName, Index);
					return false;
				}
				std::string ModuleName = Entry.GetString();
				if (SeenModules.insert(ModuleName).second)
					OutModules.push_back(std::move(ModuleName));
			}
			return true;
		}

		auto ReadEnabledRootModules(
			FJsonNodeView Root,
			std::vector<std::string>& OutModules,
			std::string* OutError
		) -> bool
		{
			std::unordered_set<std::string> SeenModules;
			if (Root.Contains("BaseModules"))
			{
				if (!ReadModuleArray(
					Root.GetView("BaseModules"), "BaseModules", OutModules, SeenModules, OutError))
					return false;
			}
			else
			{
				const FJsonNodeView ModuleDirs = Root.GetView("ModuleDirs");
				if (ModuleDirs.IsValid() && !ModuleDirs.IsObject())
				{
					if (OutError) *OutError = "Project descriptor field 'ModuleDirs' must be an object.";
					return false;
				}
				ModuleDirs.ForEachObjectMember([&](std::string_view ModuleName, FJsonNodeView) {
					std::string Name(ModuleName);
					if (SeenModules.insert(Name).second) OutModules.push_back(std::move(Name));
				});
			}

			const FJsonNodeView ExtraModules = Root.GetView("ExtraModules");
			if (!ExtraModules.IsValid()) return true;
			if (!ExtraModules.IsObject())
			{
				if (OutError) *OutError = "Project descriptor field 'ExtraModules' must be an object.";
				return false;
			}
			const FJsonNodeView RuntimeModules = ExtraModules.GetView(DURIN_RUNTIME_VARIANT);
			if (!RuntimeModules.IsValid()) return true;
			if (!RuntimeModules.IsObject())
			{
				if (OutError) *OutError = std::format(
					"Project descriptor field 'ExtraModules.{}' must be an object.",
					DURIN_RUNTIME_VARIANT);
				return false;
			}
			if (!RuntimeModules.Contains("Modules")) return true;
			return ReadModuleArray(
				RuntimeModules.GetView("Modules"),
				std::format("ExtraModules.{}.Modules", DURIN_RUNTIME_VARIANT),
				OutModules, SeenModules, OutError);
		}
	}

	auto NormalizeProjectFile(std::string_view ProjectFile) -> std::string { return Normalize(ProjectFile); }
	auto GetCurrentProject() -> const FProjectInfo* { return GCurrentProject ? &*GCurrentProject : nullptr; }
	auto HasCurrentProject() -> bool { return GCurrentProject.has_value(); }

	auto InitializeCurrentProject(const FProjectInitializationParams& Params, std::string* OutError) -> bool
	{
		GCurrentProject.reset();
		if (Params.bOpenProjectBrowser) return true;
		std::string Requested = Params.RequestedProjectFile;
		if (Requested.empty())
		{
			FProjectHistory History = MakeDefaultProjectHistory();
			std::string HistoryError;
			if (!History.Load(&HistoryError))
			{
				if (OutError) *OutError = std::move(HistoryError);
				return false;
			}
			Requested = History.GetMostRecentProjectFile();
		}
		if (Requested.empty()) return true;
		const std::string Normalized = Normalize(Requested);
		FJsonDocument Descriptor;
		FJsonParseError ParseError;
		if (!Descriptor.LoadFromFile(Normalized, &ParseError))
		{
			if (OutError) *OutError = std::format("Invalid project descriptor '{}': {}", Normalized, ParseError.Message);
			return false;
		}
		const FJsonNodeView Root = Descriptor.GetRootView();
		const std::string ProjectName = Root.GetView("ProjectName").GetString();
		if (ProjectName.empty())
		{
			if (OutError) *OutError = std::format("Project descriptor has no ProjectName: {}", Normalized);
			return false;
		}
		if (!FPaths::SetProjectFile(Normalized, OutError)) return false;
		if (!FMountPaths::ValidateDefaultMountPoints(OutError))
		{
			FPaths::SetProjectFile({});
			return false;
		}
		FProjectInfo Info;
		Info.Name = ProjectName;
		Info.ProjectFile = Normalized;
		Info.ProjectDir = std::filesystem::path(Normalized).parent_path().generic_string() + "/";
		Info.ContentDir = Info.ProjectDir + "Content/";
		Info.MountRoot = FMountPaths::ProjectContentMountRoot;
		if (!ReadEnabledRootModules(Root, Info.EnabledRootModules, OutError))
		{
			FPaths::SetProjectFile({});
			return false;
		}
		GCurrentProject = std::move(Info);
		return true;
	}

	auto RelaunchEditorForProject(std::string_view ProjectFile, std::string* OutError) -> bool
	{
		if (OutError) OutError->clear();
		GPendingEditorRelaunchArguments = ProjectFile.empty() ? "--project-browser" : std::format("--project=\"{}\"", Normalize(ProjectFile));
		RequestEngineExit();
		return true;
	}

	auto LaunchPendingEditorRelaunch(std::string* OutError) -> bool
	{
		if (!GPendingEditorRelaunchArguments) return true;
		const std::string HiddenWindowArgument = GIsWindowDisplaySuppressed ? " --hidden-window" : "";
		const std::string Arguments = std::format(
			"--wait-for-process={} {}{}", FPlatformProcess::CurrentProcessId(), *GPendingEditorRelaunchArguments, HiddenWindowArgument
		);
		GPendingEditorRelaunchArguments.reset();
		return FPlatformProcess::LaunchProcess(FPlatformProcess::ExecutablePath(), Arguments, OutError);
	}

	auto AcquireProjectEditOwnership(std::string* OutError) -> bool
	{
		if (OutError) OutError->clear();
		if (!GCurrentProject) return true;
#ifdef _WIN32
		if (GProjectEditMutex) return true;
		uint64 Hash = 14695981039346656037ull;
		for (unsigned char Character : GCurrentProject->ProjectFile)
		{
			Hash ^= static_cast<unsigned char>(
				StringUtils::ToLowerAscii(static_cast<char>(Character)));
			Hash *= 1099511628211ull;
		}
		const std::wstring Name = std::format(
			L"Local\\DurinProjectAuthoring_{:016x}", Hash);
		HANDLE Mutex = CreateMutexW(nullptr, TRUE, Name.c_str());
		if (!Mutex)
		{
			if (OutError) *OutError = std::format(
				"Could not create project edit ownership (Windows error {}).",
				GetLastError());
			return false;
		}
		if (GetLastError() == ERROR_ALREADY_EXISTS)
		{
			CloseHandle(Mutex);
			if (OutError) *OutError = std::format(
				"Another Editor process already owns project '{}'.",
				GCurrentProject->Name);
			return false;
		}
		GProjectEditMutex = Mutex;
#elif defined(__APPLE__)
		if (GProjectEditFile >= 0
			&& GProjectEditOwnerProcess == getpid()) return true;
		if (GProjectEditFile >= 0)
		{
			close(GProjectEditFile);
			GProjectEditFile = -1;
		}
		std::error_code CanonicalError;
		const std::string Identity = std::filesystem::weakly_canonical(
			GCurrentProject->ProjectFile, CanonicalError).generic_string();
		const std::string_view IdentityPath = CanonicalError
			? std::string_view(GCurrentProject->ProjectFile) : std::string_view(Identity);
		uint64 Hash = 14695981039346656037ull;
		for (const unsigned char Character : IdentityPath)
		{
			Hash ^= Character;
			Hash *= 1099511628211ull;
		}
		const std::filesystem::path LockPath = std::filesystem::temp_directory_path()
			/ std::format("DurinProjectAuthoring-{:016x}.lock", Hash);
		const int File = open(LockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
		if (File < 0)
		{
			if (OutError) *OutError = std::format(
				"Could not create project edit lock '{}': macOS error {} ({}).",
				LockPath.generic_string(), errno, std::generic_category().message(errno));
			return false;
		}
		if (flock(File, LOCK_EX | LOCK_NB) != 0)
		{
			const int Error = errno;
			close(File);
			if (OutError) *OutError = Error == EWOULDBLOCK
				? std::format("Another Editor process already owns project '{}'.",
					GCurrentProject->Name)
				: std::format("Could not acquire project edit lock: macOS error {} ({}).",
					Error, std::generic_category().message(Error));
			return false;
		}
		const std::string Owner = std::format("pid={}\nproject={}\n",
			getpid(), GCurrentProject->ProjectFile);
		(void)ftruncate(File, 0);
		(void)write(File, Owner.data(), Owner.size());
		GProjectEditFile = File;
		GProjectEditOwnerProcess = getpid();
#endif
		return true;
	}

	auto ReleaseProjectEditOwnership() -> void
	{
#ifdef _WIN32
		if (!GProjectEditMutex) return;
		ReleaseMutex(GProjectEditMutex);
		CloseHandle(GProjectEditMutex);
		GProjectEditMutex = nullptr;
#elif defined(__APPLE__)
		if (GProjectEditFile < 0) return;
		(void)flock(GProjectEditFile, LOCK_UN);
		close(GProjectEditFile);
		GProjectEditFile = -1;
		GProjectEditOwnerProcess = 0;
#endif
	}
}
