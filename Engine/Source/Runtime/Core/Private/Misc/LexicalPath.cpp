#include "Misc/LexicalPath.h"

#include <cwctype>

namespace Durin::PathUtilities
{
	namespace
	{
		auto HasParentComponent(const std::filesystem::path& Path) -> bool
		{
			return std::ranges::any_of(Path, [](const std::filesystem::path& Component) { return Component == ".."; });
		}

		auto WithoutTrailingSeparator(std::filesystem::path Path) -> std::filesystem::path
		{
			while (Path.has_relative_path() && Path.filename().empty()) Path = Path.parent_path();
			return Path;
		}

		auto ComponentsEqual(const std::filesystem::path& A, const std::filesystem::path& B) -> bool
		{
#ifdef _WIN32
			const std::wstring AText = A.generic_wstring();
			const std::wstring BText = B.generic_wstring();
			return AText.size() == BText.size() && std::ranges::equal(AText, BText, [](wchar_t Left, wchar_t Right) {
				return std::towlower(Left) == std::towlower(Right);
			});
#else
			return A == B;
#endif
		}
	}

	auto TryMakeLexicalRelativePath(
		const std::filesystem::path& Candidate,
		const std::filesystem::path& Parent,
		std::filesystem::path& OutRelative) -> bool
	{
		OutRelative.clear();
		if (Candidate.empty() || Parent.empty() || !Candidate.is_absolute() || !Parent.is_absolute()) return false;
		if (HasParentComponent(Candidate) || HasParentComponent(Parent)) return false;

		const std::filesystem::path ComparableCandidate = WithoutTrailingSeparator(Candidate);
		const std::filesystem::path ComparableParent = WithoutTrailingSeparator(Parent);
		auto CandidateIt = ComparableCandidate.begin();
		for (auto ParentIt = ComparableParent.begin(); ParentIt != ComparableParent.end(); ++ParentIt, ++CandidateIt)
		{
			if (CandidateIt == ComparableCandidate.end() || !ComponentsEqual(*CandidateIt, *ParentIt)) return false;
		}
		for (; CandidateIt != ComparableCandidate.end(); ++CandidateIt)
			OutRelative /= *CandidateIt;
		return true;
	}

	auto IsLexicalDescendantPath(
		const std::filesystem::path& Candidate,
		const std::filesystem::path& Parent,
		bool bRecursive) -> bool
	{
		std::filesystem::path Relative;
		if (!TryMakeLexicalRelativePath(Candidate, Parent, Relative) || Relative.empty()) return false;
		return bRecursive || Relative.parent_path().empty();
	}

	auto TryResolveContainedPath(
		const std::filesystem::path& Candidate,
		const std::filesystem::path& Root,
		std::filesystem::path& OutResolvedCandidate,
		std::error_code& OutError) -> bool
	{
		OutResolvedCandidate.clear();
		OutError.clear();
		if (Candidate.empty() || Root.empty() || !Candidate.is_absolute() || !Root.is_absolute())
		{
			OutError = std::make_error_code(std::errc::invalid_argument);
			return false;
		}

		const std::filesystem::path ResolvedRoot = std::filesystem::weakly_canonical(Root, OutError);
		if (OutError) return false;
		const std::filesystem::path ResolvedCandidate = std::filesystem::weakly_canonical(Candidate, OutError);
		if (OutError) return false;
		if (!IsLexicalDescendantPath(ResolvedCandidate, ResolvedRoot, true)) return false;
		OutResolvedCandidate = ResolvedCandidate;
		return true;
	}
}
