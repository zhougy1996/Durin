#include "AssetPackageInspectionInternal.h"
#include "AssetPackageCodec.h"
#include "AssetPackageFingerprintInternal.h"
#include "AssetPackageArchive.h"
#include "Asset/PackageResource.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/PackageValueCodec.h"
#include "DObject/PackagePersistence.h"
#include "Misc/FileHelper.h"
#include "Misc/MountPaths.h"

namespace Durin
{
	using AssetPrivate::MakePackageFingerprint;
	using AssetPrivate::LoadPackageBulkBytes;
	namespace
	{
		auto Error(EAssetReadError Code, std::string Message) -> FAssetReadResult { return {Code, std::move(Message)}; }
		auto ClassifyPackageIdentity(std::string_view PhysicalPath,
			FPackagePath& OutPath) -> bool
		{
			std::filesystem::path Path(PhysicalPath);
			Path.replace_extension();
			const auto Classified = FMountPaths::ClassifyAssetPath(Path);
			return Classified && FPackagePath::TryCreate(
				Classified->NormalizedVirtualPath, OutPath);
		}
	}

	namespace AssetPrivate
	{
		auto LoadPackageBulkBytes(std::string_view PhysicalPath,
			FByteBuffer& OutBytes) -> FAssetReadResult
		{
			OutBytes.clear();
			std::filesystem::path BulkPath(PhysicalPath);
			BulkPath.replace_extension(".dbulk");
			std::error_code ErrorCode;
			if (!std::filesystem::is_regular_file(BulkPath, ErrorCode))
			{
				if (ErrorCode && ErrorCode != std::errc::no_such_file_or_directory)
					return Error(EAssetReadError::IoError,
						"Failed to inspect the package bulk companion.");
				return {};
			}
			auto Loaded = FFileHelper::LoadFileToArray(BulkPath);
			if (!Loaded) return Error(EAssetReadError::IoError, Loaded.error().ToString());
			OutBytes = std::move(*Loaded);
			return {};
		}
	}

	auto ValidateAssetPackageBytes(FByteView Bytes,
		const FPackagePath& PackagePath, FByteView BulkBytes)
		-> FAssetReadResult
	{
		const AssetPrivate::FAssetPackageCodec* Codec = nullptr;
		if (auto Result = AssetPrivate::ResolveAssetPackageReader(Bytes, Codec); !Result)
			return Result;
		return Codec->Validate({.PackageBytes = Bytes, .BulkBytes = BulkBytes,
			.PackagePath = PackagePath, .PhysicalPackageBytes = Bytes.size()});
	}


	auto FAssetPackageField::TryReadString(std::string& OutValue) const -> bool
	{
		Durin::PackagePrivate::FByteReader Reader{Payload};
		return Reader.ReadString(OutValue, Durin::PackagePrivate::MaximumPackageStringBytes) && Reader.Offset == Payload.size();
	}

	namespace
	{
		auto ReadInspectedObjectReference(
			Durin::PackagePrivate::FByteReader& Reader,
			FAssetPackageObjectReference& OutValue) -> bool
		{
			OutValue = {};
			uint8 Kind = 0;
			if (!Reader.Read(Kind) || Kind > 2) return false;
			OutValue.Kind = static_cast<EAssetPackageObjectReferenceKind>(Kind);
			if (OutValue.Kind == EAssetPackageObjectReferenceKind::Null) return true;
			if (OutValue.Kind == EAssetPackageObjectReferenceKind::Internal)
				return Reader.Read(OutValue.ObjectId) && OutValue.ObjectId != 0;
			std::string PathString;
			return Reader.ReadString(PathString, Durin::PackagePrivate::MaximumPackageStringBytes)
				&& FObjectPath::TryCreate(PathString, OutValue.ExternalPath);
		}
	}

	auto FAssetPackageField::TryReadObjectReference(
		FAssetPackageObjectReference& OutValue) const -> bool
	{
		Durin::PackagePrivate::FByteReader Reader{Payload};
		return ReadInspectedObjectReference(Reader, OutValue)
			&& Reader.Offset == Payload.size();
	}

	auto FAssetPackageField::TryReadObjectReferenceArray(
		std::vector<FAssetPackageObjectReference>& OutValues) const -> bool
	{
		OutValues.clear();
		Durin::PackagePrivate::FByteReader Reader{Payload};
		uint64 Count = 0;
		if (!Reader.Read(Count) || Count > 10000000) return false;
		OutValues.reserve(static_cast<size_t>(Count));
		for (uint64 Index = 0; Index < Count; ++Index)
		{
			FAssetPackageObjectReference Value;
			if (!ReadInspectedObjectReference(Reader, Value)) return false;
			OutValues.push_back(std::move(Value));
		}
		return Reader.Offset == Payload.size();
	}

	auto FAssetPackageField::TryReadBulkDataStorageDescriptor(
		FPackageBulkStorageDescriptor& OutValue, bool bValidateInlinePayload) const -> bool
	{
		OutValue = {};
		if (Kind != DurinCodeGen::EPropertyGenFlags::BulkData
			|| !ObjectPackage::IsSupportedPackageReaderVersion(SourceFormatVersion)) return false;
		Durin::PackagePrivate::FByteReader Reader{Payload};
		uint32 Version = 0;
		uint8 Placement = 0;
		uint8 Reserved = 0;
		uint16 Alignment = 0;
		uint32 ElementSize = 0;
		uint64 FieldIndex = 0;
		uint64 HashLow = 0;
		uint64 HashHigh = 0;
		if (!Reader.Read(Version) || Version != 1
			|| !Reader.Read(Placement) || Placement > 1
			|| !Reader.Read(Reserved) || Reserved != 0
			|| !Reader.Read(Alignment) || Alignment == 0 || Alignment > 4096
			|| (Alignment & (Alignment - 1)) != 0
			|| !Reader.Read(ElementSize) || ElementSize == 0
			|| !Reader.Read(FieldIndex) || FieldIndex == 0
			|| !Reader.Read(OutValue.PayloadId)
			|| !Reader.Read(HashLow) || !Reader.Read(HashHigh)
			|| !Reader.Read(OutValue.LogicalByteCount)
			|| !Reader.Read(OutValue.StoredByteCount)
			|| !Reader.Read(OutValue.SegmentOffset)) return false;
		OutValue.ContentHash = {HashLow, HashHigh};
		OutValue.StorageKind = Placement == 0
			? EPackageBulkStorageKind::Inline
			: EPackageBulkStorageKind::External;
		OutValue.Alignment = Alignment;
		if (!OutValue.PayloadId.IsValid() || OutValue.ContentHash.IsZero()
			|| OutValue.LogicalByteCount != OutValue.StoredByteCount) return false;
		if (Placement == 1)
			return OutValue.SegmentOffset % Alignment == 0
				&& Reader.Offset == Payload.size();
		if (Alignment != 1 || OutValue.SegmentOffset != 0
			|| Reader.Offset > Payload.size()
			|| OutValue.StoredByteCount != Payload.size() - Reader.Offset) return false;
		return !bValidateInlinePayload || FXxHash128::HashBuffer(
			FByteView(Payload).subspan(Reader.Offset))
			== OutValue.ContentHash;
	}

	auto FAssetPackageField::TryReadEditorBulkDataStorageDescriptor(
		FPackageBulkStorageDescriptor& OutValue) const -> bool
	{
		return TryReadBulkDataStorageDescriptor(OutValue);
	}

	namespace
	{
		auto ReadInspectedStructFields(Durin::PackagePrivate::FByteReader& Reader, uint32 SourceFormatVersion,
			std::vector<FAssetPackageField>& OutFields) -> bool
		{
			std::string StructName;
			uint64 FieldCount = 0;
			if (!Reader.ReadString(StructName, Durin::PackagePrivate::MaximumPackageStringBytes)
				|| !Reader.Read(FieldCount) || FieldCount > 100000) return false;
			OutFields.reserve(static_cast<size_t>(FieldCount));
			for (uint64 Index = 0; Index < FieldCount; ++Index)
			{
				FAssetPackageField Field;
				uint8 FieldKind = 0;
				uint64 PayloadSize = 0;
				if (!Reader.ReadString(Field.DeclaringClass, Durin::PackagePrivate::MaximumPackageStringBytes)
					|| !Reader.ReadString(Field.Name, Durin::PackagePrivate::MaximumPackageStringBytes)
					|| !Reader.Read(FieldKind)
					|| !Reader.ReadString(Field.TypeSignature, Durin::PackagePrivate::MaximumPackageStringBytes)
					|| !Reader.Read(PayloadSize)
					|| Reader.Offset > Reader.Bytes.size()
					|| PayloadSize > Reader.Bytes.size() - Reader.Offset) return false;
				Field.Kind = static_cast<DurinCodeGen::EPropertyGenFlags>(FieldKind);
				Field.SourceFormatVersion = SourceFormatVersion;
				Field.Payload.resize(static_cast<size_t>(PayloadSize));
				if (PayloadSize != 0
					&& !Reader.ReadBytes(Field.Payload.data(), static_cast<size_t>(PayloadSize)))
					return false;
				OutFields.push_back(std::move(Field));
			}
			return true;
		}
	}

	auto FAssetPackageField::TryInspectStructFields(
		std::vector<FAssetPackageField>& OutFields) const -> bool
	{
		OutFields.clear();
		if (Kind != DurinCodeGen::EPropertyGenFlags::Struct) return false;
		Durin::PackagePrivate::FByteReader Reader{Payload};
		return ReadInspectedStructFields(Reader, SourceFormatVersion, OutFields)
			&& Reader.Offset == Payload.size();
	}

	auto FAssetPackageField::TryInspectStructArray(
		std::vector<std::vector<FAssetPackageField>>& OutElements) const -> bool
	{
		OutElements.clear();
		if (Kind != DurinCodeGen::EPropertyGenFlags::Array
			|| !TypeSignature.starts_with("Array<Struct<")) return false;
		Durin::PackagePrivate::FByteReader Reader{Payload};
		uint64 Count = 0;
		if (!Reader.Read(Count) || Count > 100000 || Count > Payload.size() / 16) return false;
		std::vector<std::vector<FAssetPackageField>> Elements(static_cast<size_t>(Count));
		for (auto& Fields : Elements)
			if (!ReadInspectedStructFields(Reader, SourceFormatVersion, Fields)) return false;
		if (Reader.Offset != Payload.size()) return false;
		OutElements = std::move(Elements);
		return true;
	}

	auto FAssetPackageField::TryReadStruct(DStruct* Struct, void* OutValue) const -> bool
	{
		if (!Struct || !OutValue
			|| Struct->GetStructureSize() == 0
			|| Struct->GetStructureSize() > std::numeric_limits<uint16>::max()
			|| Struct->GetMinAlignment() == 0
			|| (Struct->GetMinAlignment() & (Struct->GetMinAlignment() - 1)) != 0
			|| TypeSignature != std::format("Struct<{}>", Struct->GetQualifiedName()))
			return false;

		FStructProperty RootProperty(
			FFieldVariant(), FName("InspectedStructValue"), EObjectFlags::NoFlags,
			EPropertyFlags::None, 1, 0, Struct);
		Durin::PackagePrivate::FByteReader Reader{Payload};
		return AssetPrivate::DecodeReferenceByteToolValue(
			&RootProperty, OutValue, 0, Reader, {},
			SourceFormatVersion == 0 ? ObjectPackage::DastV10FormatVersion : SourceFormatVersion)
			&& Reader.Offset == Payload.size();
	}

	namespace
	{
		auto InspectAssetPackageBytes(
			std::string_view PhysicalPath,
			FByteView Bytes,
			const FPackagePath& PackagePath,
			FAssetPackageInspection& OutInspection) -> FAssetReadResult
		{
			OutInspection = {};
			auto Result = MakePackageFingerprint(PhysicalPath, Bytes, OutInspection.Fingerprint);
			if (!Result) return Result;
			const AssetPrivate::FAssetPackageCodec* Codec = nullptr;
			if (Result = AssetPrivate::ResolveAssetPackageReader(Bytes, Codec); !Result)
				return Result;
			if (!PackagePath.IsValid())
				return Error(EAssetReadError::InvalidPath,
					"DAST v10 inspection requires a mounted package identity.");
			FByteBuffer BulkBytes;
			if (Result = LoadPackageBulkBytes(PhysicalPath, BulkBytes); !Result)
				return Result;
			const AssetPrivate::FAssetPackageReadContext Context{
				.PackageBytes = Bytes, .BulkBytes = BulkBytes,
				.PackagePath = PackagePath, .PhysicalPackageBytes = Bytes.size()};
			FAssetPackageInspection Inspection;
			Result = Codec->Inspect(Context, Inspection);
			if (!Result) return Result;
			Inspection.PhysicalPath = PhysicalPath;
			Inspection.Fingerprint = OutInspection.Fingerprint;
			OutInspection = std::move(Inspection);
			return {};
		}
	}

	auto InspectAssetPackage(std::string_view PhysicalPath, FAssetPackageInspection& OutInspection) -> FAssetReadResult
	{
		FPackagePath PackagePath;
		if (!ClassifyPackageIdentity(PhysicalPath, PackagePath))
			return Error(EAssetReadError::InvalidPath,
				"DAST v10 inspection requires a mounted package identity.");
		return InspectAssetPackage(PhysicalPath, PackagePath, OutInspection);
	}

	auto InspectAssetPackage(std::string_view PhysicalPath,
		const FPackagePath& PackagePath,
		FAssetPackageInspection& OutInspection) -> FAssetReadResult
	{
		OutInspection = {};
		auto ReadAccess = FPackageFileAccess::TryReadPackage(std::filesystem::path(PhysicalPath));
		if (!ReadAccess) return Error(EAssetReadError::InUse, "Package output is being written.");
		auto Bytes = FFileHelper::LoadFileToArray(PhysicalPath);
		if (!Bytes) return Error(EAssetReadError::IoError, Bytes.error().ToString());
		return InspectAssetPackageBytes(PhysicalPath, *Bytes, PackagePath, OutInspection);
	}


}
