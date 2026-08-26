#include "Asset/AssetImportData.h"

#include "DObject/AssetPath.h"
#include "DObject/DObjectGlobals.h"

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

		auto IsNormalizedMountedPath(std::string_view Path) -> bool
		{
			if (Path.size() < 2 || Path.size() > MaximumAssetImportStringBytes * 4
				|| Path.front() != '/' || Path.back() == '/'
				|| Path.find('\\') != std::string_view::npos
				|| Path.find("//") != std::string_view::npos) return false;
			for (size_t Begin = 1; Begin < Path.size();)
			{
				const size_t End = Path.find('/', Begin);
				const std::string_view Segment = Path.substr(
					Begin, End == std::string_view::npos ? Path.size() - Begin : End - Begin);
				if (Segment.empty() || Segment == "." || Segment == "..") return false;
				if (End == std::string_view::npos) break;
				Begin = End + 1;
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
			&& SourcePath.IsEmpty() && ContentHashLow == 0 && ContentHashHigh == 0
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
			|| !IsNormalizedMountedPath(SourcePath.Path)
			|| ContentHashLow == 0 || ContentHashHigh == 0 || ByteCount == 0)
		{
			OutError = "Source identity, role, mounted path, complete hash, size, or label is invalid.";
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
			UpdateString(Builder, Source.SourcePath.Path);
			Builder.UpdateValue(Source.ContentHashLow);
			Builder.UpdateValue(Source.ContentHashHigh);
			Builder.UpdateValue(Source.ByteCount);
			Builder.UpdateValue(Source.LastWriteTime);
		}
		return Builder.Finalize();
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
