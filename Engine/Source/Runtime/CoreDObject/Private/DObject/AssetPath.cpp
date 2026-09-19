#include "DObject/AssetPath.h"

#include "Misc/MountPaths.h"
#include "Misc/StringHelper.h"

namespace Durin
{
	namespace
	{
		auto FailPath(EObjectPathError Code, EObjectPathPart Part, std::string_view Subject,
			size_t MaximumBytes = 0, size_t ComponentIndex = 0) -> FObjectOperationResult
		{
			FObjectError Error;
			Error.Code = Code;
			Error.Part = Part;
			Error.Subject = Subject;
			Error.ActualBytes = Subject.size();
			Error.MaximumBytes = MaximumBytes;
			Error.ComponentIndex = ComponentIndex;
			return {std::move(Error)};
		}
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
		auto ValidateComponent(std::string_view Component, EObjectPathPart Part,
			size_t MaximumBytes = MaximumObjectPathComponentBytes, size_t Index = 0) -> FObjectOperationResult
		{
			if (Component.empty()) return FailPath(EObjectPathError::EmptyComponent, Part, Component, 0, Index);
			if (Component.size() > MaximumBytes) return FailPath(EObjectPathError::ComponentTooLong, Part, Component, MaximumBytes, Index);
			if (!IsValidUtf8(Component)) return FailPath(EObjectPathError::InvalidUtf8, Part, Component, 0, Index);
			if (Component == "." || Component == ".." || Component.find_first_of("/\\.:") != std::string_view::npos)
				return FailPath(EObjectPathError::ReservedSeparator, Part, Component, 0, Index);
			return {};
		}
		auto ValidatePackageSyntax(std::string_view InPath) -> FObjectOperationResult
		{
			constexpr auto Part = EObjectPathPart::Package;
			if (InPath.empty() || InPath.front() != '/') return FailPath(EObjectPathError::NotAbsolute, Part, InPath);
			if (InPath.size() >= FName::MaxSize) return FailPath(EObjectPathError::InternedNameTooLong, Part, InPath, FName::MaxSize - 1);
			if (InPath.size() > MaximumObjectPathBytes) return FailPath(EObjectPathError::PathTooLong, Part, InPath, MaximumObjectPathBytes);
			if (InPath.back() == '/') return FailPath(EObjectPathError::MissingPackageName, Part, InPath);
			if (!IsValidUtf8(InPath)) return FailPath(EObjectPathError::InvalidUtf8, Part, InPath);
			if (InPath.find_first_of("\\.:") != std::string_view::npos) return FailPath(EObjectPathError::PackageSuffix, Part, InPath);
			size_t Index = 0;
			for (size_t Start = 1; Start < InPath.size(); ++Index)
			{
				const size_t End = InPath.find('/', Start);
				const auto Segment = InPath.substr(Start, End == std::string_view::npos ? InPath.size() - Start : End - Start);
				if (auto Result = ValidateComponent(Segment, EObjectPathPart::PackageSegment, MaximumObjectPathComponentBytes, Index); !Result) return Result;
				Start = End == std::string_view::npos ? InPath.size() : End + 1;
			}
			return {};
		}
		auto CompareFolded(std::string_view Left, std::string_view Right) -> std::strong_ordering
		{
			const size_t Count = std::min(Left.size(), Right.size());
			for (size_t Index = 0; Index < Count; ++Index) { const char A = StringUtils::ToLowerAscii(Left[Index]); const char B = StringUtils::ToLowerAscii(Right[Index]); if (A < B) return std::strong_ordering::less; if (A > B) return std::strong_ordering::greater; }
			return Left.size() <=> Right.size();
		}
	}

	auto FPackagePath::TryCreateWithDiagnostic(std::string_view InPath, FPackagePath& OutPath) -> FObjectOperationResult
	{
		if (auto Result = Validate(InPath); !Result) return Result;
		OutPath = FPackagePath(FName(InPath, -1));
		return {};
	}
	auto FPackagePath::TryCreateProjectContent(std::string_view InPath, FPackagePath& OutPath) -> bool
	{
		if (auto Result = ValidatePackageSyntax(InPath); !Result) return false;
		if (!InPath.starts_with(FMountPaths::ProjectContentMountRoot))
			return false;
		OutPath = FPackagePath(FName(InPath, -1));
		return true;
	}
	auto FPackagePath::Validate(std::string_view InPath) -> FObjectOperationResult
	{
		if (auto Result = ValidatePackageSyntax(InPath); !Result) return Result;
		const FMountLookupResult Lookup = FMountPaths::FindMountForVirtualPath(InPath);
		if (Lookup) return {};
		auto Result = FailPath(EObjectPathError::MountLookupFailed, EObjectPathPart::Package, InPath);
		Result.Error.MountError = Lookup.Error;
		return Result;
	}
	auto FPackagePath::ToString() const -> std::string { return Path.IsNone() ? std::string{} : Path.ToString(); }
	auto FPackagePath::GetView() const -> std::string_view { return Path.IsNone() ? std::string_view{} : Path.GetComparisonNameEntry()->MakeView(); }
	auto FPackagePath::operator<=>(const FPackagePath& Other) const -> std::strong_ordering { return CompareFolded(GetView(), Other.GetView()); }

	auto FTopLevelAssetPath::TryCreateWithDiagnostic(std::string_view InPath, FTopLevelAssetPath& OutPath) -> FObjectOperationResult
	{
		if (InPath.size() > MaximumObjectPathBytes) return FailPath(EObjectPathError::PathTooLong, EObjectPathPart::Asset, InPath, MaximumObjectPathBytes);
		if (InPath.find(':') != std::string_view::npos) return FailPath(EObjectPathError::SubobjectSuffix, EObjectPathPart::Asset, InPath);
		const size_t Slash = InPath.find_last_of('/'); const size_t Dot = InPath.find('.', Slash == std::string_view::npos ? 0 : Slash + 1);
		if (Dot == std::string_view::npos || InPath.find('.', Dot + 1) != std::string_view::npos) return FailPath(EObjectPathError::AssetSeparator, EObjectPathPart::Asset, InPath);
		FPackagePath Package; if (auto Result = FPackagePath::TryCreateWithDiagnostic(InPath.substr(0, Dot), Package); !Result) return Result; return TryCreateWithDiagnostic(Package, InPath.substr(Dot + 1), OutPath);
	}
	auto FTopLevelAssetPath::TryCreateWithDiagnostic(const FPackagePath& InPackagePath, std::string_view InAssetName, FTopLevelAssetPath& OutPath) -> FObjectOperationResult
	{
		if (!InPackagePath.IsValid()) return FailPath(EObjectPathError::MissingPackagePath, EObjectPathPart::Asset, InAssetName);
		if (auto Result = ValidateComponent(InAssetName, EObjectPathPart::AssetName, FName::MaxSize - 1); !Result) return Result;
		if (InPackagePath.GetView().size() + 1 + InAssetName.size() > MaximumObjectPathBytes) return FailPath(EObjectPathError::PathTooLong, EObjectPathPart::Asset, InPackagePath.ToString() + "." + std::string(InAssetName), MaximumObjectPathBytes);
		FTopLevelAssetPath Candidate; Candidate.PackagePath = InPackagePath; Candidate.AssetName = FName(InAssetName, -1); OutPath = std::move(Candidate); return {};
	}
	auto FTopLevelAssetPath::GetAssetName() const -> std::string_view { return AssetName.IsNone() ? std::string_view{} : AssetName.GetComparisonNameEntry()->MakeView(); }
	auto FTopLevelAssetPath::AppendTo(std::string& Out) const -> void { if (!IsValid()) return; Out.append(PackagePath.GetView()); Out.push_back('.'); Out.append(GetAssetName()); }
	auto FTopLevelAssetPath::ToString() const -> std::string { std::string Result; if (IsValid()) { Result.reserve(PackagePath.GetView().size() + 1 + GetAssetName().size()); AppendTo(Result); } return Result; }
	auto FTopLevelAssetPath::operator<=>(const FTopLevelAssetPath& Other) const -> std::strong_ordering { if (const auto Order = PackagePath <=> Other.PackagePath; Order != 0) return Order; return CompareFolded(GetAssetName(), Other.GetAssetName()); }

	auto FSubobjectPathView::FIterator::operator*() const -> std::string_view { const size_t End = Path.find('.', Offset); return Path.substr(Offset, End == std::string_view::npos ? Path.size() - Offset : End - Offset); }
	auto FSubobjectPathView::FIterator::operator++() -> FIterator& { const size_t End = Path.find('.', Offset); Offset = End == std::string_view::npos ? Path.size() : End + 1; return *this; }
	auto FSubobjectPathView::size() const -> size_t { return Path.empty() ? 0 : 1 + std::ranges::count(Path, '.'); }
	auto FSubobjectPathView::operator[](size_t Index) const -> std::string_view { auto It = begin(); while (Index-- > 0 && It != end()) ++It; return It == end() ? std::string_view{} : *It; }

	auto FObjectPath::TryCreateWithDiagnostic(std::string_view InPath, FObjectPath& OutPath) -> FObjectOperationResult
	{
		if (InPath.size() > MaximumObjectPathBytes) return FailPath(EObjectPathError::PathTooLong, EObjectPathPart::Object, InPath, MaximumObjectPathBytes);
		const size_t Colon = InPath.find(':'); if (Colon != std::string_view::npos && InPath.find(':', Colon + 1) != std::string_view::npos) return FailPath(EObjectPathError::MultipleSubobjectSeparators, EObjectPathPart::Object, InPath);
		FTopLevelAssetPath Asset; if (auto Result = FTopLevelAssetPath::TryCreateWithDiagnostic(InPath.substr(0, Colon), Asset); !Result) return Result;
		std::string Suffix; size_t Index = 0;
		if (Colon != std::string_view::npos)
		{
			const std::string_view View = InPath.substr(Colon + 1); if (View.empty() || View.front() == '.' || View.back() == '.' || View.find("..") != std::string_view::npos) return FailPath(EObjectPathError::EmptySubobject, EObjectPathPart::Subobject, View);
			for (size_t Start = 0; Start < View.size();) { const size_t End = View.find('.', Start); const auto Name = View.substr(Start, End == std::string_view::npos ? View.size() - Start : End - Start); if (auto Result = ValidateComponent(Name, EObjectPathPart::Subobject, MaximumObjectPathComponentBytes, Index++); !Result) return Result; Start = End == std::string_view::npos ? View.size() : End + 1; }
			Suffix = View;
		}
		FObjectPath Candidate; Candidate.AssetPath = std::move(Asset); Candidate.SubobjectPath = std::move(Suffix); OutPath = std::move(Candidate); return {};
	}
	auto FObjectPath::TryCreate(const FTopLevelAssetPath& InAssetPath, std::span<const std::string> Names, FObjectPath& OutPath) -> bool
	{
		if (!InAssetPath.IsValid()) return false; std::string Suffix; size_t Index = 0;
		for (const std::string& Name : Names) { if (auto Result = ValidateComponent(Name, EObjectPathPart::Subobject, MaximumObjectPathComponentBytes, Index++); !Result) return false; if (!Suffix.empty()) Suffix.push_back('.'); Suffix += Name; }
		if (InAssetPath.ToString().size() + (Suffix.empty() ? 0 : 1 + Suffix.size()) > MaximumObjectPathBytes) return false;
		FObjectPath Candidate; Candidate.AssetPath = InAssetPath; Candidate.SubobjectPath = std::move(Suffix); OutPath = std::move(Candidate); return true;
	}
	auto FObjectPath::TryCreate(const FTopLevelAssetPath& InAssetPath, FSubobjectPathView Names, FObjectPath& OutPath) -> bool
	{
		if (!InAssetPath.IsValid()) return false;
		std::string Suffix; size_t Index = 0;
		for (const std::string_view Name : Names)
		{
			if (auto Result = ValidateComponent(Name, EObjectPathPart::Subobject, MaximumObjectPathComponentBytes, Index++); !Result) return false;
			if (!Suffix.empty()) Suffix.push_back('.');
			Suffix.append(Name);
		}
		const size_t AssetPathBytes = InAssetPath.GetPackagePath().GetView().size()
			+ 1 + InAssetPath.GetAssetName().size();
		if (AssetPathBytes + (Suffix.empty() ? 0 : 1 + Suffix.size()) > MaximumObjectPathBytes)
			return false;
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
