#include "ProfilingToolService.h"

#include "Dialogs/FileDialog.h"
#include "HAL/PlatformProcess.h"
#include "Json/Json.h"
#include "Profiling/Profiling.h"

namespace Durin
{
	namespace
	{
		constexpr std::string_view ToolsManifestPath =
			"Tools/DurinDevTool/durin_dev_tool/bootstrap/thirdparty/tracy-tools.json";
		constexpr std::string_view ClientManifestPath =
			"Tools/DurinDevTool/durin_dev_tool/bootstrap/thirdparty/tracy.json";

		auto LoadManifest(
			const std::filesystem::path& ManifestPath,
			FJsonDocument& OutDocument,
			std::string& OutError
		) -> bool
		{
			FJsonParseError ParseError;
			if (OutDocument.LoadFromFile(ManifestPath.generic_string(), &ParseError)) return true;
			OutError = std::format(
				"Could not read Tracy manifest \"{}\": {} (line {}, column {}).",
				ManifestPath.generic_string(),
				ParseError.Message.empty() ? "file is missing or unreadable" : ParseError.Message,
				ParseError.Line,
				ParseError.Column
			);
			return false;
		}

		auto ReadRequiredFiles(FJsonNodeView PlatformSource) -> std::vector<std::string>
		{
			std::vector<std::string> RequiredFiles;
			const FJsonNodeView RequiredFilesNode = PlatformSource.GetView("required_files");
			for (size_t Index = 0; Index < RequiredFilesNode.Num(); ++Index)
			{
				std::string RequiredFile;
				if (RequiredFilesNode.GetView(Index).GetValue(RequiredFile) && !RequiredFile.empty())
					RequiredFiles.emplace_back(std::move(RequiredFile));
			}
			return RequiredFiles;
		}

		auto FormatUnavailableDiagnostic(const FTracyToolStatus& Status) -> std::string
		{
			std::string Diagnostic = Status.Diagnostic;
			if (!Status.ExpectedVersion.empty())
				Diagnostic += std::format("\nExpected Tracy version: {}.", Status.ExpectedVersion);
			if (!Status.ProfilerPath.empty())
				Diagnostic += std::format("\nResolved profiler path: \"{}\".", Status.ProfilerPath);
			if (!Status.RepairCommand.empty())
				Diagnostic += std::format("\nRepair with: {}", Status.RepairCommand);
			return Diagnostic;
		}

		auto JoinFileNames(const std::vector<std::string>& FileNames) -> std::string
		{
			std::string Result;
			for (const std::string& FileName : FileNames)
			{
				if (!Result.empty()) Result += ", ";
				Result += FileName;
			}
			return Result;
		}
	}

	FProfilingToolService::FProfilingToolService(std::filesystem::path InRootDirectory)
		: RootDirectory(std::filesystem::absolute(std::move(InRootDirectory)).lexically_normal())
	{
	}

	auto FProfilingToolService::QueryStatus() const -> FTracyToolStatus
	{
		FTracyToolStatus Status;
		FJsonDocument ToolsManifest;
		if (!LoadManifest(RootDirectory / ToolsManifestPath, ToolsManifest, Status.Diagnostic)) return Status;

		const FJsonNodeView ToolsRoot = ToolsManifest.GetRootView();
		Status.ExpectedVersion = ToolsRoot.GetView("version").GetString();
		Status.RepairCommand = ToolsRoot.GetView("repair_command").GetString();
		const std::string SourceDirectory = ToolsRoot.GetView("source_dir").GetString();
		const FJsonNodeView Win64Source = ToolsRoot.GetView("source").GetView("platforms").GetView("Win64");
		if (
			ToolsRoot.GetView("name").GetString() != "tracy-tools"
			|| ToolsRoot.GetView("kind").GetString() != "tool_package"
			|| Status.ExpectedVersion.empty()
			|| SourceDirectory.empty()
		)
		{
			Status.Diagnostic = std::format(
				"Tracy tools manifest is malformed: \"{}\".",
				(RootDirectory / ToolsManifestPath).generic_string()
			);
			return Status;
		}

		Status.bManifestValid = true;
		Status.bPlatformSupported = Win64Source.IsObject();
		Status.PackagePath = (RootDirectory / SourceDirectory).lexically_normal().generic_string();
		Status.ProfilerPath =
			(std::filesystem::path(Status.PackagePath) / "tracy-profiler.exe").generic_string();
		if (!Status.bPlatformSupported)
		{
			Status.Diagnostic = "The managed Tracy tools package does not support Win64.";
			return Status;
		}

		FJsonDocument ClientManifest;
		if (!LoadManifest(RootDirectory / ClientManifestPath, ClientManifest, Status.Diagnostic)) return Status;
		Status.ClientVersion = ClientManifest.GetRootView().GetView("source").GetView("tag").GetString();
		if (Status.ClientVersion.starts_with('v')) Status.ClientVersion.erase(0, 1);
		Status.bVersionMatches = Status.ClientVersion == Status.ExpectedVersion;
		if (!Status.bVersionMatches)
		{
			Status.Diagnostic = std::format(
				"Tracy client version {} does not match managed tool version {}.",
				Status.ClientVersion.empty() ? "<missing>" : Status.ClientVersion,
				Status.ExpectedVersion
			);
			return Status;
		}

		for (const std::string& RequiredFile : ReadRequiredFiles(Win64Source))
		{
			if (!std::filesystem::is_regular_file(std::filesystem::path(Status.PackagePath) / RequiredFile))
				Status.MissingFiles.emplace_back(RequiredFile);
		}
		if (!Status.MissingFiles.empty())
		{
			Status.Diagnostic = std::format(
				"The managed Tracy {} installation is incomplete; missing: {}.",
				Status.ExpectedVersion,
				JoinFileNames(Status.MissingFiles)
			);
			return Status;
		}

		Status.bAvailable = true;
		Status.Diagnostic = std::format(
			"Tracy Profiler {} is ready at \"{}\".",
			Status.ExpectedVersion,
			Status.ProfilerPath
		);
		if (const char* TracyPort = std::getenv("TRACY_PORT"))
			Status.Diagnostic += "\n" + Profiling::FormatPortOverrideDiagnostic(TracyPort);
		return Status;
	}

	auto FProfilingToolService::GetCaptureDirectory() const -> std::string
	{
		return (RootDirectory / "Build" / "Profiling" / "Tracy").lexically_normal().generic_string();
	}

	auto FProfilingToolService::BuildCaptureArguments(std::string_view CapturePath) -> std::string
	{
		return std::format("\"{}\"", std::filesystem::path(CapturePath).lexically_normal().generic_string());
	}

	auto FProfilingToolService::LaunchProfiler(std::string* OutError) const -> bool
	{
		const FTracyToolStatus Status = QueryStatus();
		if (!Status.bAvailable)
		{
			if (OutError) *OutError = FormatUnavailableDiagnostic(Status);
			return false;
		}

		std::string LaunchError;
		if (FPlatformProcess::LaunchProcess(Status.ProfilerPath, {}, &LaunchError)) return true;
		if (OutError)
			*OutError = std::format(
				"{}\nExpected Tracy version: {}.\nRepair with: {}",
				LaunchError,
				Status.ExpectedVersion,
				Status.RepairCommand
			);
		return false;
	}

	auto FProfilingToolService::OpenCapture(std::string_view CapturePath, std::string* OutError) const -> bool
	{
		const std::filesystem::path Capture = std::filesystem::absolute(CapturePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Capture) || Capture.extension() != ".tracy")
		{
			if (OutError)
				*OutError = std::format("Tracy capture does not exist or is not a .tracy file: \"{}\".", Capture.generic_string());
			return false;
		}

		const FTracyToolStatus Status = QueryStatus();
		if (!Status.bAvailable)
		{
			if (OutError) *OutError = FormatUnavailableDiagnostic(Status);
			return false;
		}

		std::string LaunchError;
		if (FPlatformProcess::LaunchProcess(
			Status.ProfilerPath,
			BuildCaptureArguments(Capture.generic_string()),
			&LaunchError
		)) return true;
		if (OutError)
			*OutError = std::format(
				"{}\nCapture: \"{}\".\nExpected Tracy version: {}.\nRepair with: {}",
				LaunchError,
				Capture.generic_string(),
				Status.ExpectedVersion,
				Status.RepairCommand
			);
		return false;
	}

	auto FProfilingToolService::SelectAndOpenCapture(void* ParentWindowHandle, std::string* OutError) const -> bool
	{
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ParentWindowHandle;
		Request.Title = "Open a Tracy Capture";
		Request.Filters = {{"Tracy Capture", "*.tracy"}};
		const std::filesystem::path CaptureDirectory(GetCaptureDirectory());
		Request.InitialDirectory = std::filesystem::is_directory(CaptureDirectory)
			? CaptureDirectory.generic_string()
			: RootDirectory.generic_string();
		const FFileDialogResult Result = OpenFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return true;
		if (Result.Status == EFileDialogStatus::Error)
		{
			if (OutError) *OutError = Result.ErrorMessage;
			return false;
		}
		return OpenCapture(Result.FilePath, OutError);
	}

	auto FProfilingToolService::OpenCaptureDirectory(std::string* OutError) const -> bool
	{
		const std::filesystem::path CaptureDirectory(GetCaptureDirectory());
		std::error_code Error;
		std::filesystem::create_directories(CaptureDirectory, Error);
		if (Error)
		{
			if (OutError)
				*OutError = std::format(
					"Could not create Tracy capture directory \"{}\": {}.",
					CaptureDirectory.generic_string(),
					Error.message()
				);
			return false;
		}
		return FPlatformProcess::OpenPath(CaptureDirectory.generic_string(), OutError);
	}
}
