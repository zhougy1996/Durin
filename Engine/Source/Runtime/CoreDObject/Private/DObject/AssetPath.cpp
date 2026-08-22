#include "DObject/AssetPath.h"

#include "Misc/Failure.h"

#include "Misc/Paths.h"

namespace Durin
{
	namespace
	{
	}

	auto FAssetPath::TryCreate(std::string_view InPath, FAssetPath& OutPath, std::string* OutError) -> bool
	{
		if (!IsValid(InPath, OutError)) return false;
		OutPath = FAssetPath(std::string(InPath));
		return true;
	}

	auto FAssetPath::IsValid(std::string_view InPath, std::string* OutError) -> bool
	{
		if (InPath.empty() || InPath.front() != '/') return Fail("Asset path must be absolute.", OutError);
		if (InPath.back() == '/') return Fail("Asset path must name an asset.", OutError);
		if (InPath.find('\\') != std::string_view::npos) return Fail("Asset path must use forward slashes.", OutError);
		if (InPath.find(':') != std::string_view::npos) return Fail("Asset path cannot contain an object suffix.", OutError);
		if (std::filesystem::path(InPath).has_extension()) return Fail("Asset path cannot contain a file extension.", OutError);

		size_t Start = 1;
		while (Start < InPath.size())
		{
			const size_t End = InPath.find('/', Start);
			const std::string_view Segment = InPath.substr(Start, End == std::string_view::npos ? InPath.size() - Start : End - Start);
			if (Segment.empty() || Segment == "." || Segment == "..") return Fail("Asset path contains an invalid segment.", OutError);
			Start = End == std::string_view::npos ? InPath.size() : End + 1;
		}

		const PathUtilities::FMountLookupResult Lookup = PathUtilities::FindMountForVirtualPath(InPath);
		if (!Lookup) return Fail(Lookup.Message, OutError);
		return true;
	}
}
