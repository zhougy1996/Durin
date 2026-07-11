#include "Misc/Project.h"

#include "CoreGlobals.h"
#include "HAL/PlatformProcess.h"
#include "Json/Json.h"
#include "Misc/Paths.h"
#include "Yaml/Yaml.h"

namespace Durin
{
	namespace
	{
		std::vector<FProjectInfo> GRegisteredProjects;
		std::optional<FProjectInfo> GCurrentProject;

		auto Normalize(const std::filesystem::path& Path) -> std::string
		{
			std::error_code Error;
			const std::filesystem::path Absolute = std::filesystem::absolute(Path, Error).lexically_normal();
			return (Error ? Path.lexically_normal() : Absolute).generic_string();
		}

		auto LoadRegisteredProjects(std::string* OutError) -> bool
		{
			GRegisteredProjects.clear();
			FJsonDocument Registry;
			FJsonParseError ParseError;
			const std::string RegistryFile = FPaths::EngineDir() + "Configs/RegisteredProjects.json";
			if (!Registry.LoadFromFile(RegistryFile, &ParseError))
			{
				if (OutError) *OutError = std::format("Could not load registered projects: {}", ParseError.Message);
				return false;
			}
			Registry.GetRootView().GetView("Projects").ForEachObjectMember([](std::string_view Name, FJsonNodeView Node) {
				const std::string RelativeFile = Node.GetString();
				if (Name.empty() || RelativeFile.empty()) return;
				FProjectInfo Info;
				Info.Name = Name;
				Info.ProjectFile = Normalize(std::filesystem::path(FPaths::RootDir()) / RelativeFile);
				Info.ProjectDir = std::filesystem::path(Info.ProjectFile).parent_path().generic_string() + "/";
				Info.ContentDir = Info.ProjectDir + "Content/";
				Info.MountRoot = std::format("/{}/", Name);
				GRegisteredProjects.push_back(std::move(Info));
			});
			return true;
		}

		auto FindArgument(std::span<const std::string_view> Arguments, std::string_view Prefix) -> std::string
		{
			for (const std::string_view Argument : Arguments)
				if (Argument.starts_with(Prefix)) return std::string(Argument.substr(Prefix.size()));
			return {};
		}
	}

	auto NormalizeProjectFile(std::string_view ProjectFile) -> std::string { return Normalize(ProjectFile); }
	auto GetRegisteredProjects() -> const std::vector<FProjectInfo>& { return GRegisteredProjects; }
	auto GetCurrentProject() -> const FProjectInfo* { return GCurrentProject ? &*GCurrentProject : nullptr; }
	auto HasCurrentProject() -> bool { return GCurrentProject.has_value(); }

	auto InitializeCurrentProject(std::span<const std::string_view> Arguments, std::string* OutError) -> bool
	{
		GCurrentProject.reset();
		if (!LoadRegisteredProjects(OutError)) return false;
		const bool bForceBrowser = std::ranges::find(Arguments, std::string_view("--project-browser")) != Arguments.end();
		std::string Requested = FindArgument(Arguments, "--project=");
		if (bForceBrowser) return true;
		if (Requested.empty())
		{
			FYamlDocument Session;
			if (Session.LoadFromFile(FPaths::LaunchDir() + "LevelEditorSession.yaml"))
				Requested = Session.GetRootView().GetView("RecentProject").GetString();
		}
		if (Requested.empty()) return true;
		const std::string Normalized = Normalize(Requested);
		const auto Found = std::ranges::find_if(GRegisteredProjects, [&Normalized](const FProjectInfo& Info) {
			return Normalize(Info.ProjectFile) == Normalized;
		});
		if (Found == GRegisteredProjects.end())
		{
			if (OutError) *OutError = "The requested project is not registered in this workspace.";
			return false;
		}
		FJsonDocument Descriptor;
		FJsonParseError ParseError;
		if (!Descriptor.LoadFromFile(Found->ProjectFile, &ParseError) || Descriptor.GetRootView().GetView("ProjectName").GetString() != Found->Name)
		{
			if (OutError) *OutError = std::format("Invalid project descriptor: {}", Found->ProjectFile);
			return false;
		}
		GCurrentProject = *Found;
		return true;
	}

	auto RelaunchEditorForProject(std::string_view ProjectFile, std::string* OutError) -> bool
	{
		const std::string Arguments = ProjectFile.empty() ? "--project-browser" : std::format("--project=\"{}\"", Normalize(ProjectFile));
		if (!FPlatformProcess::LaunchProcess(FPlatformProcess::ExecutablePath(), Arguments, OutError)) return false;
		RequestEngineExit();
		return true;
	}
}
