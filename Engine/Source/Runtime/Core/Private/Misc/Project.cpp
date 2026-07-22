#include "Misc/Project.h"

#include "CoreGlobals.h"
#include "HAL/PlatformProcess.h"
#include "Json/Json.h"
#include "Misc/Paths.h"
#include "Misc/ProjectHistory.h"

namespace Durin
{
	namespace
	{
		std::optional<FProjectInfo> GCurrentProject;
		std::optional<std::string> GPendingEditorRelaunchArguments;

		auto Normalize(const std::filesystem::path& Path) -> std::string
		{
			std::error_code Error;
			const std::filesystem::path Absolute = std::filesystem::absolute(Path, Error).lexically_normal();
			return (Error ? Path.lexically_normal() : Absolute).generic_string();
		}

		auto FindArgument(std::span<const std::string_view> Arguments, std::string_view Prefix) -> std::string
		{
			for (const std::string_view Argument : Arguments)
				if (Argument.starts_with(Prefix)) return std::string(Argument.substr(Prefix.size()));
			return {};
		}
	}

	auto NormalizeProjectFile(std::string_view ProjectFile) -> std::string { return Normalize(ProjectFile); }
	auto GetCurrentProject() -> const FProjectInfo* { return GCurrentProject ? &*GCurrentProject : nullptr; }
	auto HasCurrentProject() -> bool { return GCurrentProject.has_value(); }

	auto InitializeCurrentProject(std::span<const std::string_view> Arguments, std::string* OutError) -> bool
	{
		GCurrentProject.reset();
		const bool bForceBrowser = std::ranges::find(Arguments, std::string_view("--project-browser")) != Arguments.end();
		std::string Requested = FindArgument(Arguments, "--project=");
		if (Requested.empty())
		{
			for (size_t Index = 0; Index + 1 < Arguments.size(); ++Index)
				if (Arguments[Index] == "--project") { Requested = Arguments[Index + 1]; break; }
		}
		if (bForceBrowser) return true;
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
}
