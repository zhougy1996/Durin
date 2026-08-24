#pragma once

#include "AssetCoreAPI.h"
#include "Asset/Result.h"
#include "Asset/EditorBulkData.h"
#include "Asset/EditorBulkDataStorageTypes.h"
#include "Asset/CookedAsset.h"
#include "DObject/Archive.h"
#include "DObject/CoreDObject.h"
#include "DObject/ObjectPtr.h"
#include "DObject/Property.h"
#include "DObject/SoftObjectPtr.h"

namespace Durin::Asset
{
	namespace SaveOverridePrivate
	{
		template<typename T> struct TObjectPtrType
		{
			static constexpr bool Value = false;
		};
		template<typename T> struct TObjectPtrType<TObjectPtr<T>>
		{
			static constexpr bool Value = true;
			using ElementType = T;
		};
		template<typename T> struct TSoftObjectPtrType
		{
			static constexpr bool Value = false;
		};
		template<typename T> struct TSoftObjectPtrType<TSoftObjectPtr<T>>
		{
			static constexpr bool Value = true;
			using ElementType = T;
		};

		template<typename T>
		consteval auto ReflectedKind() -> DurinCodeGen::EPropertyGenFlags
		{
			using TValue = std::remove_cvref_t<T>;
			using enum DurinCodeGen::EPropertyGenFlags;
			if constexpr (std::is_same_v<TValue, bool>) return Bool;
			else if constexpr (std::is_same_v<TValue, int8>) return Int8;
			else if constexpr (std::is_same_v<TValue, int16>) return Int16;
			else if constexpr (std::is_same_v<TValue, int32>) return Int32;
			else if constexpr (std::is_same_v<TValue, int64>) return Int64;
			else if constexpr (std::is_same_v<TValue, uint8>) return UInt8;
			else if constexpr (std::is_same_v<TValue, uint16>) return UInt16;
			else if constexpr (std::is_same_v<TValue, uint32>) return UInt32;
			else if constexpr (std::is_same_v<TValue, uint64>) return UInt64;
			else if constexpr (std::is_same_v<TValue, float>) return Float;
			else if constexpr (std::is_same_v<TValue, double>) return Double;
			else if constexpr (std::is_same_v<TValue, std::string>) return String;
			else if constexpr (std::is_same_v<TValue, FName>) return Name;
			else if constexpr (std::is_same_v<TValue, FGuid>) return Guid;
			else if constexpr (std::is_same_v<TValue, std::byte>) return Byte;
			else if constexpr (TObjectPtrType<TValue>::Value) return Object;
			else if constexpr (TSoftObjectPtrType<TValue>::Value) return SoftObject;
			else if constexpr (std::is_enum_v<TValue>) return Enum;
			else if constexpr (requires { TValue::StaticStruct(); }) return Struct;
			else return None;
		}

		template<typename T> auto ReflectedStruct() -> DStruct*
		{
			using TValue = std::remove_cvref_t<T>;
			if constexpr (requires { TValue::StaticStruct(); }) return TValue::StaticStruct();
			else return nullptr;
		}

		template<typename T> auto ReferencedClass() -> DClass*
		{
			using TValue = std::remove_cvref_t<T>;
			if constexpr (TObjectPtrType<TValue>::Value)
				return TObjectPtrType<TValue>::ElementType::StaticClass();
			else if constexpr (TSoftObjectPtrType<TValue>::Value)
				return TSoftObjectPtrType<TValue>::ElementType::StaticClass();
			else return nullptr;
		}
	}

	enum class EAssetPackageSaveDomain : uint8
	{
		Authored,
		Cooked,
	};

	enum class EPropertySaveOverrideKind : uint8
	{
		Omit,
		Replace,
	};

	struct FPropertySaveOverride
	{
		const FProperty* Property = nullptr;
		EPropertySaveOverrideKind Kind = EPropertySaveOverrideKind::Omit;
		FPropertyValueSnapshot Replacement;
	};

	// Owns the effective reflected values and omissions for one object in one save.
	struct FObjectSaveOverride
	{
		const DObject* Object = nullptr;
		bool bOmitObject = false;
		std::vector<FPropertySaveOverride> Properties;
	};

	// Validates and owns all non-mutating object/property changes for one package save.
	class FObjectSaveOverrides
	{
	public:
		ASSETCORE_API auto AddObjectOmission(
			const DObject& Object, std::string* OutError = nullptr) -> bool;
		ASSETCORE_API auto AddPropertyOmission(
			const DObject& Object, const FProperty& Property,
			std::string* OutError = nullptr) -> bool;

		template<typename T>
		auto AddPropertyValue(
			const DObject& Object,
			const FProperty& Property,
			const T& Replacement,
			std::string* OutError = nullptr) -> bool
		{
			return AddPropertyValueRaw(
				Object, Property, &Replacement, sizeof(T), alignof(T),
				SaveOverridePrivate::ReflectedKind<T>(),
				SaveOverridePrivate::ReflectedStruct<T>(),
				SaveOverridePrivate::ReferencedClass<T>(), OutError);
		}

		ASSETCORE_API auto FindObject(const DObject& Object) const -> const FObjectSaveOverride*;
		ASSETCORE_API auto IsEmpty() const -> bool { return Objects.empty(); }
		auto GetObjects() const -> std::span<const FObjectSaveOverride> { return Objects; }

	private:
		ASSETCORE_API auto AddPropertyValueRaw(
			const DObject& Object,
			const FProperty& Property,
			const void* Replacement,
			size_t ReplacementSize,
			size_t ReplacementAlignment,
			DurinCodeGen::EPropertyGenFlags ReplacementKind,
			const DStruct* ReplacementStruct,
			const DClass* ReplacementClass,
			std::string* OutError) -> bool;
		auto FindMutableObject(const DObject& Object) -> FObjectSaveOverride*;

		std::vector<FObjectSaveOverride> Objects;
	};

	enum class EAssetPackageWriterSelection : uint8
	{
		Ordinary,
		DastV4,
		DastV5,
	};

	struct FAssetPackageSaveOptions
	{
		EAssetPackageWriterSelection WriterSelection =
			EAssetPackageWriterSelection::Ordinary;
	};

	ASSETCORE_API auto CanonicalizeAssetPackageForCook(
		std::span<const std::byte> Bytes,
		std::vector<std::byte>& OutBytes
	) -> FAssetResult;

	struct FAssetPackageSerializationOptions
	{
		EAssetPackageWriterSelection WriterSelection =
			EAssetPackageWriterSelection::Ordinary;
		EAssetPackageSaveDomain Domain = EAssetPackageSaveDomain::Authored;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		bool bRetainEditorOnlyData = false;
		std::shared_ptr<const FObjectSaveOverrides> SaveOverrides;
		std::function<bool(const DObject*, const FProperty*)> PropertyFilter;
		std::vector<FEditorBulkDataStoragePayload>* EditorBulkDataStoragePayloads = nullptr;
	};

	enum class EAssetBundleSavePhase : uint8
	{
		CreateDirectories,
		PublishCompanion,
		StagePackage,
		PublishPackage,
		PublishRootPackage,
		PublishRegistry
	};

	struct FAssetBundleSaveOptions
	{
		DPackage* RootPackage = nullptr;
		EAssetPackageWriterSelection WriterSelection =
			EAssetPackageWriterSelection::Ordinary;
		std::function<bool(EAssetBundleSavePhase, size_t)> ShouldFail;
	};

	ASSETCORE_API auto SerializeAssetPackageBytes(
		DPackage* Package,
		std::vector<std::byte>& OutBytes,
		const FAssetPackageSerializationOptions& Options = {}
	) -> FAssetResult;
	ASSETCORE_API auto SavePackagesAtomically(
		std::span<DPackage* const> Packages,
		const FAssetBundleSaveOptions& Options = {}
	) -> FAssetResult;
	ASSETCORE_API auto AdmitAssetPackageToCatalog(
		const FAssetPath& Path
	) -> FAssetResult;
} // namespace Durin::Asset
