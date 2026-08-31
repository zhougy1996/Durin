#include "DObject/AssetPath.h"

#include "Misc/MountPaths.h"

namespace Durin
{
	namespace
	{
		auto FailPath(std::string Message, std::string* OutError) -> bool
		{
			if (OutError) *OutError = std::move(Message);
			return false;
		}

		auto IsValidUtf8(std::string_view Value) -> bool
		{
			for (size_t Index = 0; Index < Value.size();)
			{
				const uint8 Lead = static_cast<uint8>(Value[Index++]);
				if (Lead < 0x80) continue;
				uint32 Code = 0;
				size_t Continuations = 0;
				if ((Lead & 0xe0) == 0xc0) { Code = Lead & 0x1f; Continuations = 1; }
				else if ((Lead & 0xf0) == 0xe0) { Code = Lead & 0x0f; Continuations = 2; }
				else if ((Lead & 0xf8) == 0xf0) { Code = Lead & 0x07; Continuations = 3; }
				else return false;
				if (Index + Continuations > Value.size()) return false;
				for (size_t Part = 0; Part < Continuations; ++Part)
				{
					const uint8 Next = static_cast<uint8>(Value[Index++]);
					if ((Next & 0xc0) != 0x80) return false;
					Code = (Code << 6) | (Next & 0x3f);
				}
				if ((Continuations == 1 && Code < 0x80)
					|| (Continuations == 2 && Code < 0x800)
					|| (Continuations == 3 && Code < 0x10000)
					|| Code > 0x10ffff
					|| (Code >= 0xd800 && Code <= 0xdfff)) return false;
			}
			return true;
		}

		auto ValidateComponent(std::string_view Component, std::string_view Kind,
			std::string* OutError) -> bool
		{
			if (Component.empty())
				return FailPath(std::format("{} cannot be empty.", Kind), OutError);
			if (Component.size() > MaximumObjectPathComponentBytes)
				return FailPath(std::format("{} exceeds the {} byte component limit.",
					Kind, MaximumObjectPathComponentBytes), OutError);
			if (!IsValidUtf8(Component))
				return FailPath(std::format("{} must be valid UTF-8.", Kind), OutError);
			if (Component == "." || Component == ".."
				|| Component.find_first_of("/\\.:") != std::string_view::npos)
			{
				return FailPath(std::format("{} contains a reserved separator.", Kind), OutError);
			}
			return true;
		}

		auto ValidatePackageSyntax(std::string_view InPath,
			std::string* OutError) -> bool
		{
			if (InPath.empty() || InPath.front() != '/')
				return FailPath("Package path must be absolute.", OutError);
			if (InPath.size() > MaximumObjectPathBytes)
				return FailPath(std::format("Package path exceeds the {} byte path limit.",
					MaximumObjectPathBytes), OutError);
			if (InPath.back() == '/')
				return FailPath("Package path must name a package.", OutError);
			if (!IsValidUtf8(InPath))
				return FailPath("Package path must be valid UTF-8.", OutError);
			if (InPath.find_first_of("\\.:") != std::string_view::npos)
				return FailPath("Package path cannot contain an object or file suffix.", OutError);

			size_t Start = 1;
			while (Start < InPath.size())
			{
				const size_t End = InPath.find('/', Start);
				const std::string_view Segment = InPath.substr(
					Start, End == std::string_view::npos ? InPath.size() - Start : End - Start);
				if (!ValidateComponent(Segment, "Package path segment", OutError)) return false;
				Start = End == std::string_view::npos ? InPath.size() : End + 1;
			}
			return true;
		}
	}

	auto FPackagePath::TryCreate(
		std::string_view InPath, FPackagePath& OutPath,
		std::string* OutError) -> bool
	{
		if (!IsValid(InPath, OutError)) return false;
		OutPath = FPackagePath(std::string(InPath));
		return true;
	}

	auto FPackagePath::TryCreateProjectContent(
		std::string_view InPath, FPackagePath& OutPath,
		std::string* OutError) -> bool
	{
		if (!ValidatePackageSyntax(InPath, OutError)) return false;
		if (!InPath.starts_with(FMountPaths::ProjectContentMountRoot))
			return FailPath("Deferred package path must use the /Game mount.", OutError);
		OutPath = FPackagePath(std::string(InPath));
		return true;
	}

	auto FPackagePath::IsValid(std::string_view InPath, std::string* OutError) -> bool
	{
		if (!ValidatePackageSyntax(InPath, OutError)) return false;
		const FMountLookupResult Lookup = FMountPaths::FindMountForVirtualPath(InPath);
		if (!Lookup) return FailPath(Lookup.Message, OutError);
		return true;
	}

	auto FTopLevelAssetPath::TryCreate(
		std::string_view InPath, FTopLevelAssetPath& OutPath,
		std::string* OutError) -> bool
	{
		if (InPath.size() > MaximumObjectPathBytes)
			return FailPath(std::format("Top-level asset path exceeds the {} byte path limit.",
				MaximumObjectPathBytes), OutError);
		if (InPath.find(':') != std::string_view::npos)
			return FailPath("Top-level asset path cannot contain a subobject suffix.", OutError);
		const size_t Slash = InPath.find_last_of('/');
		const size_t Dot = InPath.find('.', Slash == std::string_view::npos ? 0 : Slash + 1);
		if (Dot == std::string_view::npos || InPath.find('.', Dot + 1) != std::string_view::npos)
			return FailPath("Top-level asset path must contain exactly one asset separator.", OutError);

		FPackagePath Package;
		if (!FPackagePath::TryCreate(InPath.substr(0, Dot), Package, OutError)) return false;
		return TryCreate(Package, InPath.substr(Dot + 1), OutPath, OutError);
	}

	auto FTopLevelAssetPath::TryCreate(
		const FPackagePath& InPackagePath, std::string_view InAssetName,
		FTopLevelAssetPath& OutPath, std::string* OutError) -> bool
	{
		if (!InPackagePath.IsValid())
			return FailPath("Top-level asset path requires a package path.", OutError);
		if (!ValidateComponent(InAssetName, "Top-level asset name", OutError)) return false;
		std::string Canonical = std::format("{}.{}", InPackagePath.GetView(), InAssetName);
		if (Canonical.size() > MaximumObjectPathBytes)
			return FailPath(std::format("Top-level asset path exceeds the {} byte path limit.",
				MaximumObjectPathBytes), OutError);

		FTopLevelAssetPath Candidate;
		Candidate.PackagePath = InPackagePath;
		Candidate.AssetName = InAssetName;
		Candidate.CanonicalPath = std::move(Canonical);
		OutPath = std::move(Candidate);
		return true;
	}

	auto FObjectPath::TryCreate(
		std::string_view InPath, FObjectPath& OutPath,
		std::string* OutError) -> bool
	{
		if (InPath.size() > MaximumObjectPathBytes)
			return FailPath(std::format("Object path exceeds the {} byte path limit.",
				MaximumObjectPathBytes), OutError);
		const size_t Colon = InPath.find(':');
		if (Colon != std::string_view::npos && InPath.find(':', Colon + 1) != std::string_view::npos)
			return FailPath("Object path can contain at most one subobject separator.", OutError);

		FTopLevelAssetPath Asset;
		if (!FTopLevelAssetPath::TryCreate(InPath.substr(0, Colon), Asset, OutError)) return false;
		std::vector<std::string> Names;
		if (Colon != std::string_view::npos)
		{
			const std::string_view Suffix = InPath.substr(Colon + 1);
			if (Suffix.empty())
				return FailPath("Object path subobject suffix cannot be empty.", OutError);
			if (Suffix.front() == '.' || Suffix.back() == '.'
				|| Suffix.find("..") != std::string_view::npos)
			{
				return FailPath("Object path contains an empty subobject name.", OutError);
			}
			size_t Start = 0;
			while (Start < Suffix.size())
			{
				const size_t End = Suffix.find('.', Start);
				const std::string_view Name = Suffix.substr(
					Start, End == std::string_view::npos ? Suffix.size() - Start : End - Start);
				if (!ValidateComponent(Name, "Subobject name", OutError)) return false;
				Names.emplace_back(Name);
				Start = End == std::string_view::npos ? Suffix.size() : End + 1;
			}
		}
		return TryCreate(Asset, Names, OutPath, OutError);
	}

	auto FObjectPath::TryCreate(
		const FTopLevelAssetPath& InAssetPath,
		std::span<const std::string> InSubobjectNames,
		FObjectPath& OutPath, std::string* OutError) -> bool
	{
		if (!InAssetPath.IsValid())
			return FailPath("Object path requires a top-level asset path.", OutError);
		std::string Canonical = InAssetPath.ToString();
		for (size_t Index = 0; Index < InSubobjectNames.size(); ++Index)
		{
			if (!ValidateComponent(InSubobjectNames[Index], "Subobject name", OutError)) return false;
			Canonical += Index == 0 ? ':' : '.';
			Canonical += InSubobjectNames[Index];
		}
		if (Canonical.size() > MaximumObjectPathBytes)
			return FailPath(std::format("Object path exceeds the {} byte path limit.",
				MaximumObjectPathBytes), OutError);

		FObjectPath Candidate;
		Candidate.AssetPath = InAssetPath;
		Candidate.SubobjectNames.assign(InSubobjectNames.begin(), InSubobjectNames.end());
		Candidate.CanonicalPath = std::move(Canonical);
		OutPath = std::move(Candidate);
		return true;
	}
}
