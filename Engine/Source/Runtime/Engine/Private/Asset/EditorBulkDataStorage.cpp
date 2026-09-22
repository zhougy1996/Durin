#include "Asset/EditorBulkDataStorage.h"

#include "Serialization/Archive.h"

namespace Durin
{
	namespace
	{
		auto CollectDescriptors(
			DurinCodeGen::EPropertyGenFlags Kind,
			FByteView Payload,
			std::vector<FPackageBulkStorageDescriptor>& Out,
			uint32 Depth,
			uint32 SourceFormatVersion) -> FEditorBulkDataStorageResult
		{
			auto Reject = [&](EEditorBulkDataStorageError Code, uint64 Actual = 0, uint64 Expected = 0,
				const FArchiveFailure* Archive = nullptr) {
				auto Result = std::unexpected(FEditorBulkDataStorageError{.Code = Code,
					.Depth = Depth, .SourceFormatVersion = SourceFormatVersion,
					.Actual = Actual, .Expected = Expected});
				if (Archive)
				{
					Result.error().ArchiveCode = Archive->Code;
					Result.error().ArchivePath = Archive->Path;
				}
				return std::unexpected(std::move(Result.error()));
			};
			if (Depth > 64) return Reject(EEditorBulkDataStorageError::DepthLimit, Depth, 64);
			if (Kind == DurinCodeGen::EPropertyGenFlags::BulkData)
			{
				if (!ObjectPackage::IsSupportedPackageReaderVersion(SourceFormatVersion))
					return Reject(EEditorBulkDataStorageError::UnsupportedVersion, SourceFormatVersion,
						ObjectPackage::DastV10FormatVersion);
				FAssetPackageField Field{.Kind = Kind,
					.Payload = FByteBuffer(Payload.begin(), Payload.end()),
					.SourceFormatVersion = SourceFormatVersion};
				FPackageBulkStorageDescriptor Descriptor;
				if (!Field.TryReadEditorBulkDataStorageDescriptor(Descriptor))
					return Reject(EEditorBulkDataStorageError::InvalidDescriptor, Payload.size());
				Out.push_back(std::move(Descriptor));
				return {};
			}
			if (Kind != DurinCodeGen::EPropertyGenFlags::Struct) return {};

			FCanonicalMemoryReader Reader(Payload, EArchivePurpose::BulkData);
			// Fixed Struct arrays project consecutive Struct records without a count.
			// The package codec has already validated their exact membership.
			do
			{
				std::string StructName;
				uint64 FieldCount = 0;
				Reader << StructName << FieldCount;
				if (Reader.IsError() || FieldCount > 100000)
				{
					auto Result = Reject(Reader.IsError() ? EEditorBulkDataStorageError::InvalidStructHeader
						: EEditorBulkDataStorageError::FieldLimit, FieldCount, 100000, Reader.GetFailure());
					Result.error().StructName = std::move(StructName);
					return std::unexpected(std::move(Result.error()));
				}
				for (uint64 Index = 0; Index < FieldCount; ++Index)
				{
					std::string DeclaringType, Name, Signature;
					uint8 FieldKind = 0;
					uint64 PayloadSize = 0;
					Reader << DeclaringType << Name << FieldKind << Signature << PayloadSize;
					if (Reader.IsError() || PayloadSize > Reader.GetRemainingPayloadBytes())
					{
						auto Result = Reject(EEditorBulkDataStorageError::TruncatedField,
							PayloadSize, Reader.GetRemainingPayloadBytes(), Reader.GetFailure());
						Result.error().StructName = StructName;
						Result.error().Index = Index;
						Result.error().FieldRoute.push_back(std::move(Name));
						return std::unexpected(std::move(Result.error()));
					}
					FByteBuffer FieldPayload(static_cast<size_t>(PayloadSize));
					if (PayloadSize != 0)
						Reader.SerializeRawBytes(std::as_writable_bytes(std::span(FieldPayload)));
					auto Result = Reader.IsError()
						? Reject(EEditorBulkDataStorageError::TruncatedField,
							PayloadSize, Reader.GetRemainingPayloadBytes(), Reader.GetFailure())
						: CollectDescriptors(static_cast<DurinCodeGen::EPropertyGenFlags>(FieldKind),
							FieldPayload, Out, Depth + 1, SourceFormatVersion);
					if (!Result)
					{
						Result.error().FieldRoute.insert(Result.error().FieldRoute.begin(), std::move(Name));
						return std::unexpected(std::move(Result.error()));
					}
				}
			} while (Reader.GetRemainingPayloadBytes() != 0);
			return {};
		}
	}

	auto FormatEditorBulkDataStorageError(const FEditorBulkDataStorageError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EEditorBulkDataStorageError::None: return {};
		case EEditorBulkDataStorageError::DepthLimit: return "Authored bulk inspection exceeded the struct depth limit.";
		case EEditorBulkDataStorageError::UnsupportedVersion: return "Authored bulk inspection requires DAST v10 field metadata.";
		case EEditorBulkDataStorageError::InvalidDescriptor: return "Inspected authored bulk descriptor is invalid.";
		case EEditorBulkDataStorageError::InvalidStructHeader:
		case EEditorBulkDataStorageError::FieldLimit: return "Inspected authored struct payload header is invalid.";
		case EEditorBulkDataStorageError::TruncatedField: return "Inspected authored struct field is truncated.";
		case EEditorBulkDataStorageError::FileSystem: return "Editor bulk companion could not be inspected.";
		}
		return "Editor bulk storage inspection failed.";
	}

	auto InspectEditorBulkDataCompanionPaths(
		const std::filesystem::path& PackagePath,
		const FAssetPackageInspection& Inspection) -> std::expected<std::vector<std::filesystem::path>, FEditorBulkDataStorageError>
	{
		std::vector<FFilePath> OutPaths;
		std::vector<FPackageBulkStorageDescriptor> Descriptors;
		if (auto Result = InspectEditorBulkDataStorageDescriptors(Inspection); !Result)
		{
			Result.error().Path = PackagePath;
			return std::unexpected(std::move(Result.error()));
		}
		else { Descriptors = std::move(*Result); }
		if (std::ranges::any_of(Descriptors, [](const auto& Descriptor) {
			return Descriptor.StorageKind == EPackageBulkStorageKind::External;
		}))
		{
			std::filesystem::path Path = PackagePath;
			Path.replace_extension(".dbulk");
			OutPaths.push_back(std::move(Path));
		}
		return OutPaths;
	}

	auto InspectEditorBulkDataStorageDescriptors(
		const FAssetPackageInspection& Inspection) -> std::expected<std::vector<FPackageBulkStorageDescriptor>, FEditorBulkDataStorageError>
	{
		std::vector<FPackageBulkStorageDescriptor> OutDescriptors;
		for (const FAssetPackageObjectInspection& Object : Inspection.Objects)
			for (const FAssetPackageField& Field : Object.Fields)
				if (auto Result = CollectDescriptors(Field.Kind, Field.Payload, OutDescriptors, 0,
						Field.SourceFormatVersion); !Result)
				{
					Result.error().ObjectId = Object.Id;
					Result.error().ObjectPath = Object.ObjectPath;
					Result.error().FieldRoute.insert(Result.error().FieldRoute.begin(), Field.Name);
					return std::unexpected(std::move(Result.error()));
				}
		return OutDescriptors;
	}

	auto InspectOrphanedEditorBulkDataCompanionPaths(
		const std::filesystem::path& PackagePath,
		const FAssetPackageInspection& Inspection) -> std::expected<std::vector<std::filesystem::path>, FEditorBulkDataStorageError>
	{
		std::vector<FFilePath> OutPaths;
		std::vector<std::filesystem::path> Referenced;
		if (auto Result = InspectEditorBulkDataCompanionPaths(PackagePath, Inspection); !Result)
			return std::unexpected(std::move(Result.error()));
		else { Referenced = std::move(*Result); }
		std::filesystem::path Candidate = PackagePath;
		Candidate.replace_extension(".dbulk");
		std::error_code ErrorCode;
		if (std::filesystem::is_regular_file(Candidate, ErrorCode)
			&& !std::ranges::contains(Referenced, Candidate))
			OutPaths.push_back(Candidate);
		if (ErrorCode)
			return std::unexpected(FEditorBulkDataStorageError{.Code = EEditorBulkDataStorageError::FileSystem,
				.Path = std::move(Candidate), .SystemError = ErrorCode});
		return OutPaths;
	}
}
