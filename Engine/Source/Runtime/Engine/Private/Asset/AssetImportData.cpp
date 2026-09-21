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
	}

	auto FormatAssetImportDataError(const FAssetImportDataError& Error) -> std::string
	{
		if (!Error.Message.empty()) return Error.Message;
		switch (Error.Code)
		{
		case EAssetImportDataError::None: return {};
		case EAssetImportDataError::UnsupportedSchema:
			return std::format("Unsupported asset-import-data schema version {}.", Error.Actual);
		case EAssetImportDataError::TooManySources: return "Asset import source count exceeds its bound.";
		case EAssetImportDataError::NonCanonicalRoles:
			return "Asset import source roles are duplicated or not in canonical order.";
		case EAssetImportDataError::EmptySource: return "Asset import source entry is empty.";
		case EAssetImportDataError::MissingImportReference:
			return "The package main object has no valid internal AssetImportData reference.";
		case EAssetImportDataError::MissingSourceData:
			return "AssetImportData has no valid common SourceData value.";
		default: return "Source role, hint, complete hash, size, or label is invalid.";
		}
	}

	auto FAssetImportDataState::Validate() const -> FAssetImportDataResult
	{
		if (SchemaVersion != AssetImportDataSchemaVersion)
			return std::unexpected(FAssetImportDataError{.Code = EAssetImportDataError::UnsupportedSchema,
				.Actual = SchemaVersion, .Expected = AssetImportDataSchemaVersion});
		return SourceData.Validate();
	}

	auto FSourceFile::IsEmpty() const -> bool
	{
		return Role.IsNone() && DisplayLabel.empty()
			&& Hint.empty() && ContentHashLow == 0 && ContentHashHigh == 0
			&& ByteCount == 0;
	}

	auto FSourceFile::Validate() const -> FAssetImportDataResult
	{
		if (IsEmpty()) return {};
		const std::string CanonicalRole = GetCanonicalRole(Role);
		auto Reject = [&](EAssetImportDataError Code, uint64 Actual = 0, uint64 Expected = 0) {
			return std::unexpected(FAssetImportDataError{.Code = Code, .Actual = Actual,
				.Expected = Expected, .Role = Role.ToString(), .Hint = Hint,
				.DisplayLabel = DisplayLabel, .HintBase = HintBase, .ContentHash = GetContentHash()});
		};
		if (Role.GetNumber() != 0 || CanonicalRole.size() > MaximumAssetImportRoleBytes
			|| !IsIdentifier(CanonicalRole))
			return Reject(EAssetImportDataError::InvalidRole, CanonicalRole.size(), MaximumAssetImportRoleBytes);
		if (DisplayLabel.size() > MaximumAssetImportStringBytes)
			return Reject(EAssetImportDataError::InvalidLabel, DisplayLabel.size(), MaximumAssetImportStringBytes);
		if (!Hint.empty() && !IsNormalizedSourceHint(HintBase, Hint))
			return Reject(EAssetImportDataError::InvalidHint, Hint.size(), MaximumSourceHintBytes);
		if (ContentHashLow == 0 || ContentHashHigh == 0)
			return Reject(EAssetImportDataError::IncompleteHash);
		if (ByteCount == 0) return Reject(EAssetImportDataError::EmptyPayload, ByteCount, 1);
		return {};
	}

	auto FAssetImportInfo::Normalize() -> void
	{
		std::ranges::sort(Sources, [](const FSourceFile& A, const FSourceFile& B) {
			return GetCanonicalRole(A.Role) < GetCanonicalRole(B.Role);
		});
	}

	auto FAssetImportInfo::Validate() const -> FAssetImportDataResult
	{
		if (Sources.size() > MaximumAssetImportSources)
			return std::unexpected(FAssetImportDataError{.Code = EAssetImportDataError::TooManySources,
				.Actual = Sources.size(), .Expected = MaximumAssetImportSources});
		std::string PreviousRole;
		for (size_t Index = 0; Index < Sources.size(); ++Index)
		{
			const FSourceFile& Source = Sources[Index];
			if (Source.IsEmpty())
				return std::unexpected(FAssetImportDataError{.Code = EAssetImportDataError::EmptySource, .Index = Index});
			if (auto Validation = Source.Validate(); !Validation)
			{
				Validation.error().Index = Index;
				return std::unexpected(std::move(Validation.error()));
			}
			const std::string Role = GetCanonicalRole(Source.Role);
			if (!PreviousRole.empty() && PreviousRole >= Role)
				return std::unexpected(FAssetImportDataError{.Code = EAssetImportDataError::NonCanonicalRoles,
					.Index = Index, .Role = Role, .PreviousRole = PreviousRole});
			PreviousRole = Role;
		}
		return {};
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

	auto FormatSourceHintError(const FSourceHintError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case ESourceHintError::None: return {};
		case ESourceHintError::FileSystem: return Error.SystemError.message();
		case ESourceHintError::InvalidPaths:
			return Error.Operation == ESourceHintOperation::Make
				? "Source hint classification requires absolute source, project, and .dasset paths."
				: "Source hint resolution requires an absolute owning .dasset and project path.";
		case ESourceHintError::EmptyPath: return "Source or owning package filename is empty.";
		case ESourceHintError::RelativePathUnavailable: return "Project-local source could not be made package-relative.";
		case ESourceHintError::OutsideProject: return "Project-relative source hint requires a source inside the project.";
		case ESourceHintError::InvalidBase: return "Source hint base is invalid.";
		case ESourceHintError::InvalidHint: return "Source hint is not a bounded normalized platform path.";
		case ESourceHintError::EscapesProject: return "Project-relative source hint escapes the project directory.";
		}
		return {};
	}

	auto MakeSourceHint(
		std::string_view PhysicalPath,
		std::string_view OwningPackagePhysicalPath,
		std::optional<ESourceHintBase> RequestedBase) -> std::expected<FSourceHint, FSourceHintError>
	{
		const std::string ProjectPath = FPaths::ProjectDir();
		auto Reject = [&](ESourceHintError Code, ESourceHintPath Path = ESourceHintPath::Source,
			std::error_code SystemError = {}) {
			return std::unexpected(FSourceHintError{.Code = Code, .Operation = ESourceHintOperation::Make,
				.Path = Path, .Input = std::string(PhysicalPath), .PackagePath = std::string(OwningPackagePhysicalPath),
				.ProjectPath = ProjectPath, .Base = RequestedBase, .SystemError = SystemError});
		};
		if (PhysicalPath.empty() || OwningPackagePhysicalPath.empty())
		{
			return Reject(ESourceHintError::EmptyPath);
		}
		std::error_code Error;
		const std::filesystem::path Absolute = std::filesystem::absolute(
			std::filesystem::path(PhysicalPath), Error).lexically_normal();
		if (Error) return Reject(ESourceHintError::FileSystem, ESourceHintPath::Source, Error);
		const std::filesystem::path Package = std::filesystem::absolute(
			std::filesystem::path(OwningPackagePhysicalPath), Error).lexically_normal();
		if (Error) return Reject(ESourceHintError::FileSystem, ESourceHintPath::Package, Error);
		const std::filesystem::path Project = std::filesystem::absolute(
			std::filesystem::path(ProjectPath), Error).lexically_normal();
		if (Error) return Reject(ESourceHintError::FileSystem, ESourceHintPath::Project, Error);
		if (!Absolute.is_absolute() || !Package.is_absolute()
			|| !Project.is_absolute() || Package.extension() != ".dasset")
		{
			return Reject(ESourceHintError::InvalidPaths);
		}
		const bool bSourceInsideProject = IsWithinProject(Absolute, Project);
		const auto OutBase = RequestedBase.value_or(
			bSourceInsideProject
				? (IsWithinProject(Package, Project)
					? ESourceHintBase::AssetRelative
					: ESourceHintBase::ProjectRelative)
				: ESourceHintBase::Absolute);
		RequestedBase = OutBase;
		std::filesystem::path Stored;
		if (OutBase == ESourceHintBase::AssetRelative)
		{
			Stored = Absolute.lexically_relative(Package.parent_path());
			if (Stored.empty() || Stored.is_absolute())
			{
				return Reject(ESourceHintError::RelativePathUnavailable);
			}
		}
		else if (OutBase == ESourceHintBase::ProjectRelative)
		{
			if (!bSourceInsideProject)
			{
				return Reject(ESourceHintError::OutsideProject);
			}
			Stored = Absolute.lexically_relative(Project);
		}
		else if (OutBase == ESourceHintBase::Absolute) Stored = Absolute;
		else
		{
			return Reject(ESourceHintError::InvalidBase);
		}
		const std::string Candidate = Stored.lexically_normal().generic_string();
		if (!IsNormalizedSourceHint(OutBase, Candidate))
		{
			return Reject(ESourceHintError::InvalidHint);
		}
		return FSourceHint{OutBase, Candidate};
	}

	auto ResolveSourceHint(
		ESourceHintBase Base,
		std::string_view Hint,
		std::string_view OwningPackagePhysicalPath) -> std::expected<FFilePath, FSourceHintError>
	{
		const std::string ProjectPath = FPaths::ProjectDir();
		auto Reject = [&](ESourceHintError Code, ESourceHintPath Path = ESourceHintPath::Source,
			std::error_code SystemError = {}) {
			return std::unexpected(FSourceHintError{.Code = Code, .Operation = ESourceHintOperation::Resolve,
				.Path = Path, .Input = std::string(Hint), .PackagePath = std::string(OwningPackagePhysicalPath),
				.ProjectPath = ProjectPath, .Base = Base, .SystemError = SystemError});
		};
		if (!IsNormalizedSourceHint(Base, Hint))
		{
			return Reject(ESourceHintError::InvalidHint);
		}
		std::error_code Error;
		const std::filesystem::path Package = std::filesystem::absolute(
			std::filesystem::path(OwningPackagePhysicalPath), Error).lexically_normal();
		if (Error) return Reject(ESourceHintError::FileSystem, ESourceHintPath::Package, Error);
		const std::filesystem::path Project = std::filesystem::absolute(
			std::filesystem::path(ProjectPath), Error).lexically_normal();
		if (Error) return Reject(ESourceHintError::FileSystem, ESourceHintPath::Project, Error);
		if (!Package.is_absolute() || Package.extension() != ".dasset"
			|| !Project.is_absolute())
		{
			return Reject(ESourceHintError::InvalidPaths);
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
				return Reject(ESourceHintError::EscapesProject);
			}
		}
		else if (Base == ESourceHintBase::Absolute)
			Resolved = Stored.lexically_normal();
		else
		{
			return Reject(ESourceHintError::InvalidBase);
		}
		return Resolved;
	}

	DAssetImportData::DAssetImportData(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DAssetImportData::Validate() const -> FAssetImportDataResult
	{
		return GetState().Validate();
	}

	auto DAssetImportData::GetCompilationIdentity() const -> FXxHash128
	{
		FXxHash128Builder Builder;
		Builder.UpdateValue(SchemaVersion);
		Builder.UpdateValue(SourceData.GetFingerprint());
		return Builder.Finalize();
	}

	auto DAssetImportData::SetState(
		FAssetImportDataState State) -> void
	{
		require(State.SchemaVersion == AssetImportDataSchemaVersion);
		if (GetState() == State) return;
		SchemaVersion = State.SchemaVersion;
		SourceData = std::move(State.SourceData);
		if (auto* Mesh = Cast<DStaticMesh>(GetOuter()); Mesh && Mesh->GetAssetImportData() == this)
			NotifyStaticMeshCompilationMutation(*Mesh);
	}

	auto InspectAssetImportInfo(
		const FAssetPackageInspection& Inspection) -> std::expected<FAssetImportInfo, FAssetImportDataError>
	{
		const FAssetPackageField* ImportDataField =
			Inspection.FindField("AssetImportData");
		FAssetPackageObjectReference Reference;
		if (!ImportDataField || !ImportDataField->TryReadObjectReference(Reference)
			|| Reference.Kind != EAssetPackageObjectReferenceKind::Internal)
		{
			return std::unexpected(FAssetImportDataError{.Code = EAssetImportDataError::MissingImportReference});
		}
		const FAssetPackageObjectInspection* ImportDataObject =
			Inspection.FindObject(Reference.ObjectId);
		const FAssetPackageField* SourceDataField = ImportDataObject
			? ImportDataObject->FindField("SourceData") : nullptr;
		FAssetImportInfo Info;
		if (!SourceDataField
			|| !SourceDataField->TryReadStruct(FAssetImportInfo::StaticStruct(), &Info))
		{
			return std::unexpected(FAssetImportDataError{.Code = EAssetImportDataError::MissingSourceData, .ObjectId = Reference.ObjectId});
		}
		if (auto Validation = Info.Validate(); !Validation)
		{
			Validation.error().ObjectId = Reference.ObjectId;
			return std::unexpected(std::move(Validation.error()));
		}
		return Info;
	}
}
