#pragma once

#include "EngineAPI.h"
#include "Asset/EditorBulkDataStorageTypes.h"
#include "AssetRegistry/Catalog.h"
#include "AssetRegistry/PackageTypes.h"
#include "AssetRegistry/Result.h"
#include "AssetRegistry/PackageHeader.h"
#include "DObject/AssetPath.h"
#include "DObject/DObjectFwd.h"
#include "DObject/DObjectGlobals.h"

namespace Durin::Asset
{
	ENGINE_API auto ValidateAssetPackageBytes(
		std::span<const std::byte> Bytes,
		const FAssetPath& PackagePath,
		std::span<const std::byte> BulkBytes = {}
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
		std::vector<std::byte> Payload;
		uint32 SourceFormatVersion = 0;

		ENGINE_API auto TryReadString(std::string& OutValue) const -> bool;
		ENGINE_API auto TryReadStruct(DStruct* Struct, void* OutValue) const -> bool;
		ENGINE_API auto TryReadObjectReference(
			FAssetPackageObjectReference& OutValue
		) const -> bool;
		ENGINE_API auto TryReadObjectReferenceArray(
			std::vector<FAssetPackageObjectReference>& OutValues
		) const -> bool;
		ENGINE_API auto TryReadEditorBulkDataStorageDescriptor(
			FEditorBulkDataStorageDescriptor& OutValue
		) const -> bool;
		ENGINE_API auto TryReadBulkDataStorageDescriptor(
			FEditorBulkDataStorageDescriptor& OutValue
		) const -> bool;
		// Decodes the tagged fields of one reflected struct without constructing its
		// C++ value or resolving nested bulk storage.
		ENGINE_API auto TryInspectStructFields(
			std::vector<FAssetPackageField>& OutFields) const -> bool;

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
		std::string PhysicalPath;
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

	ENGINE_API auto InspectAssetPackage(
		std::string_view PhysicalPath,
		FAssetPackageInspection& OutInspection
	) -> FAssetResult;
	ENGINE_API auto InspectAssetPackage(
		std::string_view PhysicalPath,
		const FAssetPath& PackagePath,
		FAssetPackageInspection& OutInspection
	) -> FAssetResult;
} // namespace Durin::Asset
