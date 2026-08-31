#include "DObject/AssetPath.h"

#include "Misc/Paths.h"
#include "Misc/MountPaths.h"

namespace Durin
{
	namespace
	{
		auto ValidateCanonicalSyntax(std::string_view InPath,
			std::string* OutError) -> bool
		{
			if (InPath.empty() || InPath.front() != '/')
				return Fail("Asset path must be absolute.", OutError);
			if (InPath.back() == '/')
				return Fail("Asset path must name an asset.", OutError);
			if (InPath.find('\\') != std::string_view::npos)
				return Fail("Asset path must use forward slashes.", OutError);
			if (InPath.find(':') != std::string_view::npos)
				return Fail("Asset path cannot contain an object suffix.", OutError);
			if (std::filesystem::path(InPath).has_extension())
				return Fail("Asset path cannot contain a file extension.", OutError);

			size_t Start = 1;
			while (Start < InPath.size())
			{
				const size_t End = InPath.find('/', Start);
				const std::string_view Segment = InPath.substr(
					Start, End == std::string_view::npos
						? InPath.size() - Start : End - Start);
				if (Segment.empty() || Segment == "." || Segment == "..")
					return Fail("Asset path contains an invalid segment.", OutError);
				Start = End == std::string_view::npos ? InPath.size() : End + 1;
			}
			return true;
		}
	}

	auto FAssetPath::TryCreate(std::string_view InPath, FAssetPath& OutPath, std::string* OutError) -> bool
	{
		if (!IsValid(InPath, OutError)) return false;
		OutPath = FAssetPath(std::string(InPath));
		return true;
	}

	auto FAssetPath::TryCreateProjectContent(
		std::string_view InPath, FAssetPath& OutPath, std::string* OutError) -> bool
	{
		if (!ValidateCanonicalSyntax(InPath, OutError)) return false;
		if (!InPath.starts_with(FMountPaths::ProjectContentMountRoot))
			return Fail("Deferred asset path must use the /Game mount.", OutError);
		OutPath = FAssetPath(std::string(InPath));
		return true;
	}

	auto FAssetPath::IsValid(std::string_view InPath, std::string* OutError) -> bool
	{
		if (!ValidateCanonicalSyntax(InPath, OutError)) return false;

		const FMountLookupResult Lookup = FMountPaths::FindMountForVirtualPath(InPath);
		if (!Lookup) return Fail(Lookup.Message, OutError);
		return true;
	}
}
