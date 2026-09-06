#include "Asset/AssetImportData.h"
#include "Asset/SourceHint.h"
#include "StaticMesh/StaticMeshCompilation.h"

#include "DObject/AssetPath.h"
#include "DObject/DObjectGlobals.h"
#include "Misc/Paths.h"

namespace Durin
{
	namespace
	{
		auto IsIdentifier(std::string_view Value) -> bool
		{
			return !Value.empty() && Value.size() <= MaximumAssetImportStringBytes
				&& std::ranges::all_of(Value, [](unsigned char Character) {
					return std::isalnum(Character) || Character == '.' || Character == '_'
						|| Character == '-' || Character == ':' || Character == '/'
						|| Character == '+';
				});
		}

		auto GetCanonicalRole(FName Role) -> std::string
		{
			if (Role.IsNone()) return {};
			std::string Result = Role.GetComparisonNameEntry()->GetPlainNameString();
			std::ranges::transform(Result, Result.begin(), [](unsigned char Character) {
				return static_cast<char>(std::tolower(Character));
			});
			return Result;
		}

		auto IsNormalizedSourceHint(
			ESourceHintBase Base, std::string_view Hint) -> bool
		{
			if (Hint.empty() || Hint.size() > MaximumSourceHintBytes
				|| Hint.back() == '/' || Hint.find('\\') != std::string_view::npos
				|| Hint.find('\0') != std::string_view::npos
				|| Hint.find("//") != std::string_view::npos
				|| Hint.find("://") != std::string_view::npos) return false;
			const std::filesystem::path Path(Hint);
			if (Path == "." || Path.lexically_normal().generic_string() != Hint)
				return false;
			for (const std::filesystem::path& Segment : Path)
				if (Segment == ".") return false;
			if (Base == ESourceHintBase::Absolute) return Path.is_absolute();
			if (Path.is_absolute() || Path.has_root_name()) return false;
			return Base == ESourceHintBase::AssetRelative
				|| Base == ESourceHintBase::ProjectRelative;
		}

		auto IsWithinProject(
			const std::filesystem::path& Path,
			const std::filesystem::path& Project) -> bool
		{
			std::filesystem::path Relative;
			if (!FPaths::TryMakeLexicalRelativePath(Path, Project, Relative)
				|| Relative.empty() || Relative.is_absolute()) return false;
			const auto First = Relative.begin();
			return First != Relative.end() && *First != "..";
		}

		auto UpdateString(FXxHash128Builder& Builder, std::string_view Value) -> void
		{
			const uint64 Size = Value.size();
			Builder.UpdateValue(Size);
			Builder.Update(Value);
		}

		auto ValidateBaseState(
			const FAssetImportDataState& State, std::string& OutError) -> bool
		{
			if (State.SchemaVersion != AssetImportDataSchemaVersion)
			{
				OutError = std::format(
					"Unsupported asset-import-data schema version {}.", State.SchemaVersion);
				return false;
			}
			return State.SourceData.Validate(OutError);
		}

	}

	auto FSourceFile::IsEmpty() const -> bool
	{
		return Role.IsNone() && DisplayLabel.empty()
			&& Hint.empty() && ContentHashLow == 0 && ContentHashHigh == 0
			&& ByteCount == 0;
	}

	auto FSourceFile::Validate(std::string& OutError) const -> bool
	{
		if (IsEmpty())
		{
			OutError.clear();
			return true;
		}
		const std::string CanonicalRole = GetCanonicalRole(Role);
		if (Role.GetNumber() != 0 || CanonicalRole.size() > MaximumAssetImportRoleBytes
			|| !IsIdentifier(CanonicalRole)
			|| DisplayLabel.size() > MaximumAssetImportStringBytes
			|| (!Hint.empty() && !IsNormalizedSourceHint(HintBase, Hint))
			|| ContentHashLow == 0 || ContentHashHigh == 0 || ByteCount == 0)
		{
			OutError = "Source role, hint, complete hash, size, or label is invalid.";
			return false;
		}
		OutError.clear();
		return true;
	}

	auto FAssetImportInfo::Normalize() -> void
	{
		std::ranges::sort(Sources, [](const FSourceFile& A, const FSourceFile& B) {
			return GetCanonicalRole(A.Role) < GetCanonicalRole(B.Role);
		});
	}

	auto FAssetImportInfo::Validate(std::string& OutError) const -> bool
	{
		if (Sources.size() > MaximumAssetImportSources)
		{
			OutError = "Asset import source count exceeds its bound.";
			return false;
		}
		std::string PreviousRole;
		for (const FSourceFile& Source : Sources)
		{
			if (Source.IsEmpty() || !Source.Validate(OutError)) return false;
			const std::string Role = GetCanonicalRole(Source.Role);
			if (!PreviousRole.empty() && PreviousRole >= Role)
			{
				OutError = "Asset import source roles are duplicated or not in canonical order.";
				return false;
			}
			PreviousRole = Role;
		}
		OutError.clear();
		return true;
	}

	auto FAssetImportInfo::FindByRole(FName Role) const -> const FSourceFile*
	{
		const auto It = std::ranges::find(Sources, Role, &FSourceFile::Role);
		return It == Sources.end() ? nullptr : &*It;
	}

	auto FAssetImportInfo::GetFingerprint() const -> FXxHash128
	{
		FXxHash128Builder Builder;
		const uint64 Count = Sources.size();
		Builder.UpdateValue(Count);
		for (const FSourceFile& Source : Sources)
		{
			UpdateString(Builder, GetCanonicalRole(Source.Role));
			UpdateString(Builder, Source.DisplayLabel);
			Builder.UpdateValue(static_cast<uint8>(Source.HintBase));
			UpdateString(Builder, Source.Hint);
			Builder.UpdateValue(Source.ContentHashLow);
			Builder.UpdateValue(Source.ContentHashHigh);
			Builder.UpdateValue(Source.ByteCount);
		}
		return Builder.Finalize();
	}

	auto MakeSourceHint(
		std::string_view PhysicalPath,
		std::string_view OwningPackagePhysicalPath,
		ESourceHintBase& OutBase,
		std::string& OutHint,
		std::string& OutError,
		std::optional<ESourceHintBase> RequestedBase) -> bool
	{
		OutHint.clear();
		if (PhysicalPath.empty() || OwningPackagePhysicalPath.empty())
		{
			OutError = "Source or owning package filename is empty.";
			return false;
		}
		std::error_code Error;
		const std::filesystem::path Absolute = std::filesystem::absolute(
			std::filesystem::path(PhysicalPath), Error).lexically_normal();
		const std::filesystem::path Package = std::filesystem::absolute(
			std::filesystem::path(OwningPackagePhysicalPath), Error).lexically_normal();
		const std::filesystem::path Project = std::filesystem::absolute(
			std::filesystem::path(FPaths::ProjectDir()), Error).lexically_normal();
		if (Error || !Absolute.is_absolute() || !Package.is_absolute()
			|| !Project.is_absolute() || Package.extension() != ".dasset")
		{
			OutError = Error ? Error.message()
				: "Source hint classification requires absolute source, project, and .dasset paths.";
			return false;
		}
		const bool bSourceInsideProject = IsWithinProject(Absolute, Project);
		OutBase = RequestedBase.value_or(
			bSourceInsideProject
				? (IsWithinProject(Package, Project)
					? ESourceHintBase::AssetRelative
					: ESourceHintBase::ProjectRelative)
				: ESourceHintBase::Absolute);
		std::filesystem::path Stored;
		if (OutBase == ESourceHintBase::AssetRelative)
		{
			Stored = Absolute.lexically_relative(Package.parent_path());
			if (Stored.empty() || Stored.is_absolute())
			{
				OutError = "Project-local source could not be made package-relative.";
				return false;
			}
		}
		else if (OutBase == ESourceHintBase::ProjectRelative)
		{
			if (!bSourceInsideProject)
			{
				OutError = "Project-relative source hint requires a source inside the project.";
				return false;
			}
			Stored = Absolute.lexically_relative(Project);
		}
		else if (OutBase == ESourceHintBase::Absolute) Stored = Absolute;
		else
		{
			OutError = "Source hint base is invalid.";
			return false;
		}
		const std::string Candidate = Stored.lexically_normal().generic_string();
		if (!IsNormalizedSourceHint(OutBase, Candidate))
		{
			OutError = "Source hint is not a bounded normalized platform path.";
			return false;
		}
		OutHint = Candidate;
		OutError.clear();
		return true;
	}

	auto ResolveSourceHint(
		ESourceHintBase Base,
		std::string_view Hint,
		std::string_view OwningPackagePhysicalPath,
		std::string& OutPhysicalPath,
		std::string& OutError) -> bool
	{
		OutPhysicalPath.clear();
		if (!IsNormalizedSourceHint(Base, Hint))
		{
			OutError = "Source hint is not a bounded normalized platform path.";
			return false;
		}
		std::error_code Error;
		const std::filesystem::path Package = std::filesystem::absolute(
			std::filesystem::path(OwningPackagePhysicalPath), Error).lexically_normal();
		const std::filesystem::path Project = std::filesystem::absolute(
			std::filesystem::path(FPaths::ProjectDir()), Error).lexically_normal();
		if (Error || !Package.is_absolute() || Package.extension() != ".dasset"
			|| !Project.is_absolute())
		{
			OutError = Error ? Error.message()
				: "Source hint resolution requires an absolute owning .dasset and project path.";
			return false;
		}
		const std::filesystem::path Stored(Hint);
		std::filesystem::path Resolved;
		if (Base == ESourceHintBase::AssetRelative)
			Resolved = (Package.parent_path() / Stored).lexically_normal();
		else if (Base == ESourceHintBase::ProjectRelative)
		{
			Resolved = (Project / Stored).lexically_normal();
			if (!IsWithinProject(Resolved, Project))
			{
				OutError = "Project-relative source hint escapes the project directory.";
				return false;
			}
		}
		else if (Base == ESourceHintBase::Absolute)
			Resolved = Stored.lexically_normal();
		else
		{
			OutError = "Source hint base is invalid.";
			return false;
		}
		OutPhysicalPath = Resolved.generic_string();
		OutError.clear();
		return true;
	}

	DAssetImportData::DAssetImportData(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DAssetImportData::Validate(std::string& OutError) const -> bool
	{
		return ValidateBaseState(GetState(), OutError);
	}

	auto DAssetImportData::GetCompilationIdentity() const -> FXxHash128
	{
		FXxHash128Builder Builder;
		Builder.UpdateValue(SchemaVersion);
		Builder.UpdateValue(SourceData.GetFingerprint());
		return Builder.Finalize();
	}

	auto DAssetImportData::SetState(
		FAssetImportDataState State, std::string& OutError) -> bool
	{
		State.SourceData.Normalize();
		if (!ValidateBaseState(State, OutError)) return false;
		if (GetState() == State) { OutError.clear(); return true; }
		SchemaVersion = State.SchemaVersion;
		SourceData = std::move(State.SourceData);
		if (auto* Mesh = Cast<DStaticMesh>(GetOuter()); Mesh && Mesh->GetAssetImportData() == this)
			NotifyStaticMeshCompilationMutation(*Mesh);
		OutError.clear();
		return true;
	}

	auto DAssetImportData::PostLoad(std::string& OutError) -> bool
	{
		if (!Super::PostLoad(OutError)) return false;
		if (SchemaVersion == 2)
		{
			SourceData.Normalize();
			SchemaVersion = AssetImportDataSchemaVersion;
		}
		return Validate(OutError);
	}

	auto InspectAssetImportInfo(
		const FAssetPackageInspection& Inspection,
		FAssetImportInfo& OutInfo,
		std::string& OutError) -> bool
	{
		const FAssetPackageField* ImportDataField =
			Inspection.FindField("AssetImportData");
		FAssetPackageObjectReference Reference;
		if (!ImportDataField || !ImportDataField->TryReadObjectReference(Reference)
			|| Reference.Kind != EAssetPackageObjectReferenceKind::Internal)
		{
			OutError = "The package main object has no valid internal AssetImportData reference.";
			return false;
		}
		const FAssetPackageObjectInspection* ImportDataObject =
			Inspection.FindObject(Reference.ObjectId);
		const FAssetPackageField* SourceDataField = ImportDataObject
			? ImportDataObject->FindField("SourceData") : nullptr;
		FAssetImportInfo Info;
		if (!SourceDataField
			|| !SourceDataField->TryReadStruct(FAssetImportInfo::StaticStruct(), &Info)
			|| !Info.Validate(OutError))
		{
			if (OutError.empty()) OutError = "AssetImportData has no valid common SourceData value.";
			return false;
		}
		OutInfo = std::move(Info);
		OutError.clear();
		return true;
	}
}
