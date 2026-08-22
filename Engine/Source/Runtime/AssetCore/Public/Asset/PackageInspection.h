#pragma once

#include "AssetCoreAPI.h"
#include "Asset/Catalog.h"
#include "Asset/PackageTypes.h"
#include "Asset/Result.h"
#include "Asset/AuthoredBulkData.h"
#include "DObject/CoreDObject.h"

namespace Durin::Asset
{
	struct FAssetPackageHeader
	{
		std::string AssetClassName;
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		FAssetPath RedirectDestination;
		uint32 FormatVersion = 0;
		std::vector<FAssetPath> Dependencies;
		uint64 ObjectCount = 0;
		uint64 BytesRead = 0;
		uint64 FileBytesRead = 0;
	};

	ASSETCORE_API auto ReadAssetPackageHeader(
		std::string_view PhysicalPath,
		FAssetPackageHeader& OutHeader
	) -> FAssetResult;
	ASSETCORE_API auto ValidateAssetPackageBytes(
		std::span<const uint8> Bytes
	) -> FAssetResult;

	enum class EAssetPackageObjectReferenceKind : uint8
	{
		Null,
		Internal,
		External
	};

	struct FAssetPackageObjectReference
	{
		EAssetPackageObjectReferenceKind Kind = EAssetPackageObjectReferenceKind::Null;
		uint64 ObjectId = 0;
		FAssetPath ExternalPath;
	};

	struct FAssetPackageField
	{
		std::string DeclaringClass;
		std::string Name;
		DurinCodeGen::EPropertyGenFlags Kind = DurinCodeGen::EPropertyGenFlags::None;
		std::string TypeSignature;
		std::vector<uint8> Payload;
		uint32 SourceFormatVersion = 0;

		ASSETCORE_API auto TryReadString(std::string& OutValue) const -> bool;
		ASSETCORE_API auto TryReadStruct(DStruct* Struct, void* OutValue) const -> bool;
		ASSETCORE_API auto TryReadObjectReference(
			FAssetPackageObjectReference& OutValue
		) const -> bool;
		ASSETCORE_API auto TryReadObjectReferenceArray(
			std::vector<FAssetPackageObjectReference>& OutValues
		) const -> bool;
		ASSETCORE_API auto TryReadAuthoredBulkDescriptor(
			FAuthoredBulkDataDescriptor& OutValue
		) const -> bool;

		template<typename T>
		auto TryReadScalar(T& OutValue) const -> bool
		{
			static_assert(std::is_trivially_copyable_v<T>);
			if (Payload.size() != sizeof(T)) return false;
			std::memcpy(&OutValue, Payload.data(), sizeof(T));
			return true;
		}
	};

	struct FAssetPackageObjectInspection
	{
		uint64 Id = 0;
		uint64 OuterId = 0;
		std::string ClassName;
		std::string ObjectName;
		std::string ObjectPath;
		std::vector<FAssetPackageField> Fields;

		auto FindField(std::string_view Name) const -> const FAssetPackageField*
		{
			const auto It = std::ranges::find(Fields, Name, &FAssetPackageField::Name);
			return It == Fields.end() ? nullptr : &*It;
		}
	};

	struct FAssetPackageInspection
	{
		FAssetPackageHeader Header;
		FAssetPackageFingerprint Fingerprint;
		std::vector<FAssetPackageObjectInspection> Objects;

		auto FindField(std::string_view Name) const -> const FAssetPackageField*
		{
			return Objects.empty() ? nullptr : Objects.front().FindField(Name);
		}

		auto FindObject(uint64 Id) const -> const FAssetPackageObjectInspection*
		{
			if (Id == 0 || Id > Objects.size()) return nullptr;
			const FAssetPackageObjectInspection& Object =
				Objects[static_cast<size_t>(Id - 1)];
			return Object.Id == Id ? &Object : nullptr;
		}
	};

	ASSETCORE_API auto InspectAssetPackage(
		std::string_view PhysicalPath,
		FAssetPackageInspection& OutInspection
	) -> FAssetResult;
} // namespace Durin::Asset
