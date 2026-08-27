#include "Asset/AssetImportData.h"
#include "Asset/SourceFilename.h"

#include "DObject/AssetPath.h"
#include "DObject/DObjectGlobals.h"
#include "Misc/LexicalPath.h"
#include "Misc/Paths.h"

namespace Durin::AssetImport
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

		auto IsNormalizedSourceFilename(std::string_view Filename) -> bool
		{
			if (Filename.empty() || Filename.size() > MaximumSourceFilenameBytes
				|| Filename.back() == '/' || Filename.find('\\') != std::string_view::npos
				|| Filename.find('\0') != std::string_view::npos
				|| Filename.find("//") != std::string_view::npos) return false;
			const std::filesystem::path Path(Filename);
			if (Path == "." || Path.lexically_normal().generic_string() != Filename)
				return false;
			for (const std::filesystem::path& Segment : Path)
			{
				if (Segment == "." || Segment == "..") return false;
			}
			return true;
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
		return StableIdentity.empty() && Role.empty() && DisplayLabel.empty()
			&& Filename.empty() && ContentHashLow == 0 && ContentHashHigh == 0
			&& ByteCount == 0 && LastWriteTime == 0;
	}

	auto FSourceFile::Validate(std::string& OutError) const -> bool
	{
		if (IsEmpty())
		{
			OutError.clear();
			return true;
		}
		if (!IsIdentifier(StableIdentity) || !IsIdentifier(Role)
			|| DisplayLabel.size() > MaximumAssetImportStringBytes
			|| !IsNormalizedSourceFilename(Filename)
			|| ContentHashLow == 0 || ContentHashHigh == 0 || ByteCount == 0)
		{
			OutError = "Source identity, role, filename, complete hash, size, or label is invalid.";
			return false;
		}
		OutError.clear();
		return true;
	}

	auto FAssetImportInfo::Normalize() -> void
	{
		std::ranges::sort(Sources, {}, &FSourceFile::StableIdentity);
	}

	auto FAssetImportInfo::Validate(std::string& OutError) const -> bool
	{
		if (Sources.size() > MaximumAssetImportSources)
		{
			OutError = "Asset import source count exceeds its bound.";
			return false;
		}
		std::string_view Previous;
		for (const FSourceFile& Source : Sources)
		{
			if (Source.IsEmpty() || !Source.Validate(OutError)) return false;
			if (!Previous.empty() && Previous >= Source.StableIdentity)
			{
				OutError = "Asset import sources are duplicated or not in canonical identity order.";
				return false;
			}
			Previous = Source.StableIdentity;
		}
		OutError.clear();
		return true;
	}

	auto FAssetImportInfo::FindByStableIdentity(
		std::string_view StableIdentity) const -> const FSourceFile*
	{
		const auto It = std::ranges::lower_bound(Sources, StableIdentity, {},
			&FSourceFile::StableIdentity);
		return It != Sources.end() && It->StableIdentity == StableIdentity ? &*It : nullptr;
	}

	auto FAssetImportInfo::FindByRole(std::string_view Role) const -> const FSourceFile*
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
			UpdateString(Builder, Source.StableIdentity);
			UpdateString(Builder, Source.Role);
			UpdateString(Builder, Source.DisplayLabel);
			UpdateString(Builder, Source.Filename);
			Builder.UpdateValue(Source.ContentHashLow);
			Builder.UpdateValue(Source.ContentHashHigh);
			Builder.UpdateValue(Source.ByteCount);
			Builder.UpdateValue(Source.LastWriteTime);
		}
		return Builder.Finalize();
	}

	auto MakeSourceFilename(
		std::string_view PhysicalPath,
		std::string& OutFilename,
		std::string& OutError) -> bool
	{
		OutFilename.clear();
		if (PhysicalPath.empty())
		{
			OutError = "Source filename is empty.";
			return false;
		}
		std::error_code Error;
		const std::filesystem::path Absolute = std::filesystem::absolute(
			std::filesystem::path(PhysicalPath), Error).lexically_normal();
		if (Error || !Absolute.is_absolute())
		{
			OutError = Error ? Error.message() : "Source filename is not absolute.";
			return false;
		}
		std::filesystem::path Stored = Absolute;
		const std::filesystem::path Project = std::filesystem::path(
			FPaths::ProjectDir()).lexically_normal();
		std::filesystem::path Relative;
		if (!Project.empty()
			&& PathUtilities::TryMakeLexicalRelativePath(Absolute, Project, Relative)
			&& !Relative.empty())
			Stored = std::move(Relative);
		const std::string Candidate = Stored.generic_string();
		if (!IsNormalizedSourceFilename(Candidate))
		{
			OutError = "Source filename is not a bounded normalized platform path.";
			return false;
		}
		OutFilename = Candidate;
		OutError.clear();
		return true;
	}

	auto ResolveSourceFilename(
		std::string_view Filename,
		std::string& OutPhysicalPath,
		std::string& OutError) -> bool
	{
		OutPhysicalPath.clear();
		if (!IsNormalizedSourceFilename(Filename))
		{
			OutError = "Source filename is not a bounded normalized platform path.";
			return false;
		}
		const std::filesystem::path Stored(Filename);
		std::filesystem::path Resolved;
		if (Stored.is_absolute())
			Resolved = Stored;
		else
		{
			const std::filesystem::path Project = std::filesystem::path(
				FPaths::ProjectDir()).lexically_normal();
			if (Project.empty() || !Project.is_absolute())
			{
				OutError = "Project directory is unavailable for relative source filename resolution.";
				return false;
			}
			Resolved = (Project / Stored).lexically_normal();
			std::filesystem::path Relative;
			if (!PathUtilities::TryMakeLexicalRelativePath(
				Resolved, Project, Relative) || Relative.empty())
			{
				OutPhysicalPath.clear();
				OutError = "Relative source filename escapes the project directory.";
				return false;
			}
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

	auto DAssetImportData::ApplyState(
		FAssetImportDataState State, std::string& OutError) -> bool
	{
		State.SourceData.Normalize();
		if (!ValidateBaseState(State, OutError)) return false;
		SchemaVersion = State.SchemaVersion;
		SourceData = std::move(State.SourceData);
		OutError.clear();
		return true;
	}

	auto DAssetImportData::PostLoad(std::string& OutError) -> bool
	{
		if (!Super::PostLoad(OutError)) return false;
		return Validate(OutError);
	}

	auto InspectAssetImportInfo(
		const Asset::FAssetPackageInspection& Inspection,
		FAssetImportInfo& OutInfo,
		std::string& OutError) -> bool
	{
		const Asset::FAssetPackageField* ImportDataField =
			Inspection.FindField("AssetImportData");
		Asset::FAssetPackageObjectReference Reference;
		if (!ImportDataField || !ImportDataField->TryReadObjectReference(Reference)
			|| Reference.Kind != Asset::EAssetPackageObjectReferenceKind::Internal)
		{
			OutError = "The package main object has no valid internal AssetImportData reference.";
			return false;
		}
		const Asset::FAssetPackageObjectInspection* ImportDataObject =
			Inspection.FindObject(Reference.ObjectId);
		const Asset::FAssetPackageField* SourceDataField = ImportDataObject
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
