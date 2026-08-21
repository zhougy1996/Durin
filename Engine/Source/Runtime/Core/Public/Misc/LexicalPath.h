#pragma once

#include "CoreAPI.h"

namespace Durin::PathUtilities
{
	// Inputs must be non-empty, absolute, and lexically normalized. The comparison is
	// component-aware and follows the platform's native path case behavior.
	CORE_API auto TryMakeLexicalRelativePath(
		const std::filesystem::path& Candidate,
		const std::filesystem::path& Parent,
		std::filesystem::path& OutRelative) -> bool;

	CORE_API auto IsLexicalDescendantPath(
		const std::filesystem::path& Candidate,
		const std::filesystem::path& Parent,
		bool bRecursive) -> bool;

	// Resolves existing path prefixes, including symbolic links, and succeeds only
	// when Candidate remains a strict descendant of Root. Both inputs must be absolute.
	CORE_API auto TryResolveContainedPath(
		const std::filesystem::path& Candidate,
		const std::filesystem::path& Root,
		std::filesystem::path& OutResolvedCandidate,
		std::error_code& OutError) -> bool;
}
