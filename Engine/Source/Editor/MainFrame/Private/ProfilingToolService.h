#pragma once

#include "CoreMinimal.h"

namespace Durin::Profiling { struct FConnectionState; }

namespace Durin::Editor::MainFrame
{
	struct FTracyToolStatus
	{
		bool bManifestValid = false;
		bool bPlatformSupported = false;
		bool bVersionMatches = false;
		bool bAvailable = false;
		bool bCanConnect = false;
		std::string ConnectionDiagnostic;
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
		explicit FProfilingToolService(std::filesystem::path InRootDirectory);

		[[nodiscard]] auto QueryStatus() const -> FTracyToolStatus;
		[[nodiscard]] static auto BuildCaptureArguments(std::string_view CapturePath) -> std::vector<std::string>;
		[[nodiscard]] static auto BuildConnectionArguments(
			const Profiling::FConnectionState& Connection,
			std::string* OutError = nullptr
		) -> std::optional<std::vector<std::string>>;

		auto LaunchProfiler(std::string* OutError = nullptr) const -> bool;
		auto OpenCapture(std::string_view CapturePath, std::string* OutError = nullptr) const -> bool;

	private:
		std::filesystem::path RootDirectory;
	};
}
