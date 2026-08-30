#pragma once

#include "CoreAPI.h"

namespace Durin
{
	// Resolves engine and active-project locations and provides general physical-path algorithms.
	class FPaths
	{
	public:
		FPaths() = delete;

		static CORE_API auto SetProjectFile(std::string_view ProjectFile, std::string* OutError = nullptr) -> bool;
		static CORE_API auto ProjectFile() -> std::string;
		static CORE_API auto LaunchDir() -> std::string;
		static CORE_API auto LaunchSavedDir() -> std::string;
		static CORE_API auto LaunchConfigsDir() -> std::string;
		static CORE_API auto LaunchLogsDir() -> std::string;
		static CORE_API auto RootDir() -> std::string;
		static CORE_API auto EngineDir() -> std::string;
		static CORE_API auto ProjectDir() -> std::string;
		static CORE_API auto DerivedDataCacheDir() -> std::string;
		static CORE_API auto SetDerivedDataCacheDirForTests(std::string_view Directory) -> void;
		static CORE_API auto EngineContentDir() -> std::string;
		static CORE_API auto EngineBinariesDir() -> std::string;
		static CORE_API auto EngineThirdPartyRuntimeBinariesDir() -> std::string;

		// Inputs must be non-empty, absolute, and lexically normalized. The comparison is
		// component-aware and follows the platform's native path case behavior.
		static CORE_API auto TryMakeLexicalRelativePath(
			const std::filesystem::path& Candidate,
			const std::filesystem::path& Parent,
			std::filesystem::path& OutRelative) -> bool;
		static CORE_API auto IsLexicalDescendantPath(
			const std::filesystem::path& Candidate,
			const std::filesystem::path& Parent,
			bool bRecursive) -> bool;
		// Resolves existing path prefixes, including symbolic links, and succeeds only
		// when Candidate remains a strict descendant of Root. Both inputs must be absolute.
		static CORE_API auto TryResolveContainedPath(
			const std::filesystem::path& Candidate,
			const std::filesystem::path& Root,
			std::filesystem::path& OutResolvedCandidate,
			std::error_code& OutError) -> bool;
	};
}
