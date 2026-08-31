#include "DObject/AssetPath.h"

#include "Misc/MountPaths.h"
#include "Misc/StringHelper.h"

namespace Durin
{
	namespace
	{
		auto FailPath(std::string Message, std::string* OutError) -> bool { if (OutError) *OutError = std::move(Message); return false; }
		auto IsValidUtf8(std::string_view Value) -> bool
		{
			for (size_t Index = 0; Index < Value.size();)
			{
				const uint8 Lead = static_cast<uint8>(Value[Index++]); if (Lead < 0x80) continue;
				uint32 Code = 0; size_t Continuations = 0;
				if ((Lead & 0xe0) == 0xc0) { Code = Lead & 0x1f; Continuations = 1; }
				else if ((Lead & 0xf0) == 0xe0) { Code = Lead & 0x0f; Continuations = 2; }
				else if ((Lead & 0xf8) == 0xf0) { Code = Lead & 0x07; Continuations = 3; }
				else return false;
				if (Index + Continuations > Value.size()) return false;
				for (size_t Part = 0; Part < Continuations; ++Part) { const uint8 Next = static_cast<uint8>(Value[Index++]); if ((Next & 0xc0) != 0x80) return false; Code = (Code << 6) | (Next & 0x3f); }
				if ((Continuations == 1 && Code < 0x80) || (Continuations == 2 && Code < 0x800) || (Continuations == 3 && Code < 0x10000) || Code > 0x10ffff || (Code >= 0xd800 && Code <= 0xdfff)) return false;
			}
			return true;
		}
		auto ValidateComponent(std::string_view Component, std::string_view Kind, std::string* OutError, size_t MaximumBytes = MaximumObjectPathComponentBytes) -> bool
		{
			if (Component.empty()) return FailPath(std::format("{} cannot be empty.", Kind), OutError);
			if (Component.size() > MaximumBytes) return FailPath(std::format("{} exceeds the {} byte component limit.", Kind, MaximumBytes), OutError);
			if (!IsValidUtf8(Component)) return FailPath(std::format("{} must be valid UTF-8.", Kind), OutError);
			if (Component == "." || Component == ".." || Component.find_first_of("/\\.:") != std::string_view::npos) return FailPath(std::format("{} contains a reserved separator.", Kind), OutError);
			return true;
		}
		auto ValidatePackageSyntax(std::string_view InPath, std::string* OutError) -> bool
		{
			if (InPath.empty() || InPath.front() != '/') return FailPath("Package path must be absolute.", OutError);
			if (InPath.size() >= FName::MaxSize) return FailPath(std::format("Package path exceeds the {} byte interned-name limit.", FName::MaxSize - 1), OutError);
			if (InPath.size() > MaximumObjectPathBytes) return FailPath(std::format("Package path exceeds the {} byte path limit.", MaximumObjectPathBytes), OutError);
			if (InPath.back() == '/') return FailPath("Package path must name a package.", OutError);
			if (!IsValidUtf8(InPath)) return FailPath("Package path must be valid UTF-8.", OutError);
			if (InPath.find_first_of("\\.:") != std::string_view::npos) return FailPath("Package path cannot contain an object or file suffix.", OutError);
			for (size_t Start = 1; Start < InPath.size();)
			{
				const size_t End = InPath.find('/', Start); const auto Segment = InPath.substr(Start, End == std::string_view::npos ? InPath.size() - Start : End - Start);
				if (!ValidateComponent(Segment, "Package path segment", OutError)) return false;
				Start = End == std::string_view::npos ? InPath.size() : End + 1;
			}
			return true;
		}
		auto CompareFolded(std::string_view Left, std::string_view Right) -> std::strong_ordering
		{
			const size_t Count = std::min(Left.size(), Right.size());
			for (size_t Index = 0; Index < Count; ++Index) { const char A = StringUtils::ToLowerAscii(Left[Index]); const char B = StringUtils::ToLowerAscii(Right[Index]); if (A < B) return std::strong_ordering::less; if (A > B) return std::strong_ordering::greater; }
			return Left.size() <=> Right.size();
		}
	}

	auto FPackagePath::TryCreate(std::string_view InPath, FPackagePath& OutPath, std::string* OutError) -> bool { if (!IsValid(InPath, OutError)) return false; OutPath = FPackagePath(FName(InPath)); return true; }
	auto FPackagePath::TryCreateProjectContent(std::string_view InPath, FPackagePath& OutPath, std::string* OutError) -> bool { if (!ValidatePackageSyntax(InPath, OutError)) return false; if (!InPath.starts_with(FMountPaths::ProjectContentMountRoot)) return FailPath("Deferred package path must use the /Game mount.", OutError); OutPath = FPackagePath(FName(InPath)); return true; }
	auto FPackagePath::IsValid(std::string_view InPath, std::string* OutError) -> bool { if (!ValidatePackageSyntax(InPath, OutError)) return false; const FMountLookupResult Lookup = FMountPaths::FindMountForVirtualPath(InPath); return Lookup ? true : FailPath(Lookup.Message, OutError); }
	auto FPackagePath::ToString() const -> std::string { return Path.IsNone() ? std::string{} : Path.ToString(); }
	auto FPackagePath::GetView() const -> std::string_view { return Path.IsNone() ? std::string_view{} : Path.GetComparisonNameEntry()->MakeView(); }
	auto FPackagePath::operator<=>(const FPackagePath& Other) const -> std::strong_ordering { return CompareFolded(GetView(), Other.GetView()); }

	auto FTopLevelAssetPath::TryCreate(std::string_view InPath, FTopLevelAssetPath& OutPath, std::string* OutError) -> bool
	{
		if (InPath.size() > MaximumObjectPathBytes) return FailPath(std::format("Top-level asset path exceeds the {} byte path limit.", MaximumObjectPathBytes), OutError);
		if (InPath.find(':') != std::string_view::npos) return FailPath("Top-level asset path cannot contain a subobject suffix.", OutError);
		const size_t Slash = InPath.find_last_of('/'); const size_t Dot = InPath.find('.', Slash == std::string_view::npos ? 0 : Slash + 1);
		if (Dot == std::string_view::npos || InPath.find('.', Dot + 1) != std::string_view::npos) return FailPath("Top-level asset path must contain exactly one asset separator.", OutError);
		FPackagePath Package; if (!FPackagePath::TryCreate(InPath.substr(0, Dot), Package, OutError)) return false; return TryCreate(Package, InPath.substr(Dot + 1), OutPath, OutError);
	}
	auto FTopLevelAssetPath::TryCreate(const FPackagePath& InPackagePath, std::string_view InAssetName, FTopLevelAssetPath& OutPath, std::string* OutError) -> bool
	{
		if (!InPackagePath.IsValid()) return FailPath("Top-level asset path requires a package path.", OutError);
		if (!ValidateComponent(InAssetName, "Top-level asset name", OutError, FName::MaxSize - 1)) return false;
		if (InPackagePath.GetView().size() + 1 + InAssetName.size() > MaximumObjectPathBytes) return FailPath(std::format("Top-level asset path exceeds the {} byte path limit.", MaximumObjectPathBytes), OutError);
		FTopLevelAssetPath Candidate; Candidate.PackagePath = InPackagePath; Candidate.AssetName = FName(InAssetName); OutPath = std::move(Candidate); return true;
	}
	auto FTopLevelAssetPath::GetAssetName() const -> std::string_view { return AssetName.IsNone() ? std::string_view{} : AssetName.GetComparisonNameEntry()->MakeView(); }
	auto FTopLevelAssetPath::AppendTo(std::string& Out) const -> void { if (!IsValid()) return; Out.append(PackagePath.GetView()); Out.push_back('.'); Out.append(GetAssetName()); }
	auto FTopLevelAssetPath::ToString() const -> std::string { std::string Result; if (IsValid()) { Result.reserve(PackagePath.GetView().size() + 1 + GetAssetName().size()); AppendTo(Result); } return Result; }
	auto FTopLevelAssetPath::operator<=>(const FTopLevelAssetPath& Other) const -> std::strong_ordering { if (const auto Order = PackagePath <=> Other.PackagePath; Order != 0) return Order; return CompareFolded(GetAssetName(), Other.GetAssetName()); }

	auto FSubobjectPathView::FIterator::operator*() const -> std::string_view { const size_t End = Path.find('.', Offset); return Path.substr(Offset, End == std::string_view::npos ? Path.size() - Offset : End - Offset); }
	auto FSubobjectPathView::FIterator::operator++() -> FIterator& { const size_t End = Path.find('.', Offset); Offset = End == std::string_view::npos ? Path.size() : End + 1; return *this; }
	auto FSubobjectPathView::size() const -> size_t { return Path.empty() ? 0 : 1 + std::ranges::count(Path, '.'); }
	auto FSubobjectPathView::operator[](size_t Index) const -> std::string_view { auto It = begin(); while (Index-- > 0 && It != end()) ++It; return It == end() ? std::string_view{} : *It; }

	auto FObjectPath::TryCreate(std::string_view InPath, FObjectPath& OutPath, std::string* OutError) -> bool
	{
		if (InPath.size() > MaximumObjectPathBytes) return FailPath(std::format("Object path exceeds the {} byte path limit.", MaximumObjectPathBytes), OutError);
		const size_t Colon = InPath.find(':'); if (Colon != std::string_view::npos && InPath.find(':', Colon + 1) != std::string_view::npos) return FailPath("Object path can contain at most one subobject separator.", OutError);
		FTopLevelAssetPath Asset; if (!FTopLevelAssetPath::TryCreate(InPath.substr(0, Colon), Asset, OutError)) return false;
		std::string Suffix;
		if (Colon != std::string_view::npos)
		{
			const std::string_view View = InPath.substr(Colon + 1); if (View.empty() || View.front() == '.' || View.back() == '.' || View.find("..") != std::string_view::npos) return FailPath("Object path contains an empty subobject name.", OutError);
			for (size_t Start = 0; Start < View.size();) { const size_t End = View.find('.', Start); const auto Name = View.substr(Start, End == std::string_view::npos ? View.size() - Start : End - Start); if (!ValidateComponent(Name, "Subobject name", OutError)) return false; Start = End == std::string_view::npos ? View.size() : End + 1; }
			Suffix = View;
		}
		FObjectPath Candidate; Candidate.AssetPath = std::move(Asset); Candidate.SubobjectPath = std::move(Suffix); OutPath = std::move(Candidate); return true;
	}
	auto FObjectPath::TryCreate(const FTopLevelAssetPath& InAssetPath, std::span<const std::string> Names, FObjectPath& OutPath, std::string* OutError) -> bool
	{
		if (!InAssetPath.IsValid()) return FailPath("Object path requires a top-level asset path.", OutError); std::string Suffix;
		for (const std::string& Name : Names) { if (!ValidateComponent(Name, "Subobject name", OutError)) return false; if (!Suffix.empty()) Suffix.push_back('.'); Suffix += Name; }
		if (InAssetPath.ToString().size() + (Suffix.empty() ? 0 : 1 + Suffix.size()) > MaximumObjectPathBytes) return FailPath(std::format("Object path exceeds the {} byte path limit.", MaximumObjectPathBytes), OutError);
		FObjectPath Candidate; Candidate.AssetPath = InAssetPath; Candidate.SubobjectPath = std::move(Suffix); OutPath = std::move(Candidate); return true;
	}
	auto FObjectPath::TryCreate(const FTopLevelAssetPath& InAssetPath, FSubobjectPathView Names, FObjectPath& OutPath, std::string* OutError) -> bool
	{
		if (!InAssetPath.IsValid()) return FailPath("Object path requires a top-level asset path.", OutError);
		std::string Suffix;
		for (const std::string_view Name : Names)
		{
			if (!ValidateComponent(Name, "Subobject name", OutError)) return false;
			if (!Suffix.empty()) Suffix.push_back('.');
			Suffix.append(Name);
		}
		const size_t AssetPathBytes = InAssetPath.GetPackagePath().GetView().size()
			+ 1 + InAssetPath.GetAssetName().size();
		if (AssetPathBytes + (Suffix.empty() ? 0 : 1 + Suffix.size()) > MaximumObjectPathBytes)
			return FailPath(std::format("Object path exceeds the {} byte path limit.", MaximumObjectPathBytes), OutError);
		FObjectPath Candidate;
		Candidate.AssetPath = InAssetPath;
		Candidate.SubobjectPath = std::move(Suffix);
		OutPath = std::move(Candidate);
		return true;
	}
	auto FObjectPath::AppendTo(std::string& Out) const -> void { if (!IsValid()) return; AssetPath.AppendTo(Out); if (!SubobjectPath.empty()) { Out.push_back(':'); Out += SubobjectPath; } }
	auto FObjectPath::ToString() const -> std::string
	{
		std::string Result;
		if (IsValid())
		{
			Result.reserve(AssetPath.GetPackagePath().GetView().size() + 1
				+ AssetPath.GetAssetName().size()
				+ (SubobjectPath.empty() ? 0 : 1 + SubobjectPath.size()));
			AppendTo(Result);
		}
		return Result;
	}
	auto FObjectPath::operator==(const FObjectPath& Other) const -> bool { return AssetPath == Other.AssetPath && CompareFolded(SubobjectPath, Other.SubobjectPath) == 0; }
	auto FObjectPath::operator<=>(const FObjectPath& Other) const -> std::strong_ordering { if (const auto Order = AssetPath <=> Other.AssetPath; Order != 0) return Order; return CompareFolded(SubobjectPath, Other.SubobjectPath); }
}
