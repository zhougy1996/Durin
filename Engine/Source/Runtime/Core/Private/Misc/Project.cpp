#include "Misc/Project.h"

#include "CoreGlobals.h"
#include "HAL/PlatformProcess.h"
#include "Json/Json.h"
#include "Misc/Paths.h"
#include "Misc/ProjectHistory.h"

#ifdef _WIN32
	#include "Windows/WindowsPlatform.h"
#endif

namespace Durin
{
	namespace
	{
		std::optional<FProjectInfo> GCurrentProject;
		std::optional<std::string> GPendingEditorRelaunchArguments;
#ifdef _WIN32
		HANDLE GProjectAuthoringMutex = nullptr;
#endif

		auto Normalize(const std::filesystem::path& Path) -> std::string
		{
			std::error_code Error;
			const std::filesystem::path Absolute = std::filesystem::absolute(Path, Error).lexically_normal();
			return (Error ? Path.lexically_normal() : Absolute).generic_string();
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
		const std::string ProjectName = Descriptor.GetRootView().GetView("ProjectName").GetString();
		if (ProjectName.empty())
		{
			if (OutError) *OutError = std::format("Project descriptor has no ProjectName: {}", Normalized);
			return false;
		}
		if (!FPaths::SetProjectFile(Normalized, OutError)) return false;
		if (!PathUtilities::ValidateDefaultMountPoints(OutError))
		{
			FPaths::SetProjectFile({});
			return false;
		}
		FProjectInfo Info;
		Info.Name = ProjectName;
		Info.ProjectFile = Normalized;
		Info.ProjectDir = std::filesystem::path(Normalized).parent_path().generic_string() + "/";
		Info.ContentDir = Info.ProjectDir + "Content/";
		Info.MountRoot = PathUtilities::ProjectContentMountRoot;
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

	auto AcquireProjectAuthoringOwnership(std::string* OutError) -> bool
	{
		if (OutError) OutError->clear();
		if (!GCurrentProject) return true;
#ifdef _WIN32
		if (GProjectAuthoringMutex) return true;
		uint64 Hash = 14695981039346656037ull;
		for (unsigned char Character : GCurrentProject->ProjectFile)
		{
			Hash ^= static_cast<unsigned char>(std::tolower(Character));
			Hash *= 1099511628211ull;
		}
		const std::wstring Name = std::format(
			L"Local\\DurinProjectAuthoring_{:016x}", Hash);
		HANDLE Mutex = CreateMutexW(nullptr, TRUE, Name.c_str());
		if (!Mutex)
		{
			if (OutError) *OutError = std::format(
				"Could not create project authoring ownership (Windows error {}).",
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
		GProjectAuthoringMutex = Mutex;
#endif
		return true;
	}

	auto ReleaseProjectAuthoringOwnership() -> void
	{
#ifdef _WIN32
		if (!GProjectAuthoringMutex) return;
		ReleaseMutex(GProjectAuthoringMutex);
		CloseHandle(GProjectAuthoringMutex);
		GProjectAuthoringMutex = nullptr;
#endif
	}
}
