#include "Asset/EditorBulkDataStorage.h"

#include "Serialization/Archive.h"

namespace Durin::Asset
{
	namespace
	{
		auto CollectDescriptors(
			DurinCodeGen::EPropertyGenFlags Kind,
			std::span<const std::byte> Payload,
			std::vector<FEditorBulkDataStorageDescriptor>& Out,
			uint32 Depth,
			uint32 SourceFormatVersion,
			std::string* OutError) -> bool
		{
			if (Depth > 64)
				return Fail("Authored bulk inspection exceeded the struct depth limit.", OutError);
			if (Kind == DurinCodeGen::EPropertyGenFlags::BulkData)
			{
				if (SourceFormatVersion != AssetPackageV7FormatVersion)
					return Fail("Authored bulk inspection requires DAST v7 field metadata.", OutError);
				FAssetPackageField Field{.Kind = Kind,
					.Payload = std::vector<std::byte>(Payload.begin(), Payload.end()),
					.SourceFormatVersion = SourceFormatVersion};
				FEditorBulkDataStorageDescriptor Descriptor;
				if (!Field.TryReadEditorBulkDataStorageDescriptor(Descriptor))
					return Fail("Inspected authored bulk descriptor is invalid.", OutError);
				Out.push_back(std::move(Descriptor));
				return true;
			}
			if (Kind != DurinCodeGen::EPropertyGenFlags::Struct) return true;

			FCanonicalMemoryReader Reader(Payload, EArchivePurpose::BulkData);
			std::string StructName;
			uint64 FieldCount = 0;
			Reader << StructName << FieldCount;
			if (Reader.HasError() || FieldCount > 100000)
				return Fail("Inspected authored struct payload header is invalid.", OutError);
			for (uint64 Index = 0; Index < FieldCount; ++Index)
			{
				std::string DeclaringType, Name, Signature;
				uint8 FieldKind = 0;
				uint64 PayloadSize = 0;
				Reader << DeclaringType << Name << FieldKind << Signature << PayloadSize;
				if (Reader.HasError() || PayloadSize > Reader.GetRemainingPayloadBytes())
					return Fail("Inspected authored struct field is truncated.", OutError);
				std::vector<std::byte> FieldPayload(static_cast<size_t>(PayloadSize));
				if (PayloadSize != 0)
					Reader.SerializeRawBytes(std::as_writable_bytes(std::span(FieldPayload)));
				if (Reader.HasError() || !CollectDescriptors(
						static_cast<DurinCodeGen::EPropertyGenFlags>(FieldKind),
						FieldPayload, Out, Depth + 1, SourceFormatVersion, OutError))
					return false;
			}
			return Reader.Tell() == Payload.size()
				|| Fail("Inspected authored struct payload contains trailing bytes.", OutError);
		}
	}

	auto InspectEditorBulkDataCompanionPaths(
		const std::filesystem::path& PackagePath,
		const FAssetPackageInspection& Inspection,
		std::vector<std::filesystem::path>& OutPaths,
		std::string* OutError) -> bool
	{
		OutPaths.clear();
		std::vector<FEditorBulkDataStorageDescriptor> Descriptors;
		if (!InspectEditorBulkDataStorageDescriptors(Inspection, Descriptors, OutError))
			return false;
		if (std::ranges::any_of(Descriptors, [](const auto& Descriptor) {
			return Descriptor.StorageKind == EEditorBulkDataStorageKind::External;
		}))
		{
			std::filesystem::path Path = PackagePath;
			Path.replace_extension(".dbulk");
			OutPaths.push_back(std::move(Path));
		}
		if (OutError) OutError->clear();
		return true;
	}

	auto InspectEditorBulkDataStorageDescriptors(
		const FAssetPackageInspection& Inspection,
		std::vector<FEditorBulkDataStorageDescriptor>& OutDescriptors,
		std::string* OutError) -> bool
	{
		OutDescriptors.clear();
		for (const FAssetPackageObjectInspection& Object : Inspection.Objects)
			for (const FAssetPackageField& Field : Object.Fields)
				if (!CollectDescriptors(Field.Kind, Field.Payload, OutDescriptors, 0,
						Field.SourceFormatVersion, OutError))
					return false;
		if (OutError) OutError->clear();
		return true;
	}

	auto InspectOrphanedEditorBulkDataCompanionPaths(
		const std::filesystem::path& PackagePath,
		const FAssetPackageInspection& Inspection,
		std::vector<std::filesystem::path>& OutPaths,
		std::string* OutError) -> bool
	{
		OutPaths.clear();
		std::vector<std::filesystem::path> Referenced;
		if (!InspectEditorBulkDataCompanionPaths(
				PackagePath, Inspection, Referenced, OutError)) return false;
		std::filesystem::path Candidate = PackagePath;
		Candidate.replace_extension(".dbulk");
		std::error_code ErrorCode;
		if (std::filesystem::is_regular_file(Candidate, ErrorCode)
			&& std::ranges::find(Referenced, Candidate) == Referenced.end())
			OutPaths.push_back(std::move(Candidate));
		if (ErrorCode)
			return Fail("Editor bulk companion could not be inspected.", OutError);
		if (OutError) OutError->clear();
		return true;
	}
}
