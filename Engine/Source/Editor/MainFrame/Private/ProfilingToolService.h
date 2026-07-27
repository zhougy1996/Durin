#pragma once

namespace Durin
{
	struct FTracyToolStatus
	{
		bool bManifestValid = false;
		bool bPlatformSupported = false;
		bool bVersionMatches = false;
		bool bAvailable = false;
		std::string ExpectedVersion;
		std::string ClientVersion;
		std::string PackagePath;
		std::string ProfilerPath;
		std::string RepairCommand;
		std::vector<std::string> MissingFiles;
		std::string Diagnostic;
	};

	class FProfilingToolService
	{
	public:
		inline static constexpr std::string_view LaunchProfilerLabel = "Launch Tracy Profiler";
		inline static constexpr std::string_view OpenCaptureLabel = "Open Tracy Capture...";
		inline static constexpr std::string_view OpenCaptureDirectoryLabel = "Open Capture Directory";
		inline static constexpr std::string_view ShowStatusLabel = "Tool Status...";

		explicit FProfilingToolService(std::filesystem::path InRootDirectory);

		[[nodiscard]] auto QueryStatus() const -> FTracyToolStatus;
		[[nodiscard]] auto GetCaptureDirectory() const -> std::string;
		[[nodiscard]] static auto BuildCaptureArguments(std::string_view CapturePath) -> std::string;

		auto LaunchProfiler(std::string* OutError = nullptr) const -> bool;
		auto OpenCapture(std::string_view CapturePath, std::string* OutError = nullptr) const -> bool;
		auto SelectAndOpenCapture(void* ParentWindowHandle, std::string* OutError = nullptr) const -> bool;
		auto OpenCaptureDirectory(std::string* OutError = nullptr) const -> bool;

	private:
		std::filesystem::path RootDirectory;
	};
}
