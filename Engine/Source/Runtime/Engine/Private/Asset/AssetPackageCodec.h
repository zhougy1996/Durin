#pragma once

#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"

#include "EngineAPI.h"
#include "AssetSubsystemFwd.h"
#include "AssetPackageCaptureOptions.h"
#include "AssetPackageDependencyLoadPolicy.h"
#include "Asset/PackageSchema.h"
#include "DObject/PackageFormat.h"
#include "Asset/PackageBulkData.h"
#include "Asset/PackageResource.h"
#include "Asset/PackageEditing.h"

namespace Durin
{
	enum class EDefaultDeltaMode : uint8;
}

namespace Durin::AssetPrivate
{
	class IAssetPackageByteSource;

	struct FAssetPackageReadContext
	{
		FByteView PackageBytes;
		FByteView BulkBytes;
		FPackagePath PackagePath;
		uint64 PhysicalPackageBytes = 0;
		uint64 PhysicalBulkBytes = 0;
		bool bResourceBackedBulk = false;
		bool bCooked = false;
		// Retained for all external bulk fields; loading never resolves it globally.
		FPackageResourceHandle BulkResource;
		std::optional<FAssetPackageDependencyLoadPolicy> DependencyLoadPolicy;
		// Keep capture objects out of public package/object lookup. Requires a closed load policy.
		bool bPrivateGraph = false;
		DPackage* ExistingPackage = nullptr;
		const std::unordered_set<DObject*>* ExistingObjects = nullptr;
		// Ordinary loading transfers candidate completion to its component owner.
		std::function<void(std::function<std::expected<void, Durin::UnifiedError::FError>()>, std::function<void()>)> DeferCompletion;
		// Bind validated bulk entries before object construction, using the load's decoded tables.
		std::function<std::expected<void, Durin::UnifiedError::FError>(std::span<const FPackageBulkDataEntry>, FPackageResourceHandle&)> BindBulkResource;
	};

	struct FAssetPackageEncodedClosure
	{
		FByteBuffer PackageBytes;
		FByteBuffer BulkBytes;
	};

	// Defines the complete engine-owned capability set for one immutable package format.
	struct FAssetPackageCodec
	{
		std::string_view CodecId;
		uint32 FormatVersion = 0;
		bool bCanRead = false;
		bool bCanWrite = false;
		bool bCanMutate = false;

		auto (*ReadHeader)(const FAssetPackageReadContext&, FAssetPackageHeader&)
			-> std::expected<void, Durin::UnifiedError::FError> = nullptr;
		auto (*Validate)(const FAssetPackageReadContext&) -> std::expected<void, Durin::UnifiedError::FError> = nullptr;
		auto (*Inspect)(const FAssetPackageReadContext&, FAssetPackageInspection&) -> std::expected<void, Durin::UnifiedError::FError> = nullptr;
		auto (*ExtractReferences)(
			const FAssetPackageReadContext&, std::vector<FAssetReferenceEdge>&)
			-> std::expected<void, Durin::UnifiedError::FError> = nullptr;
		auto (*InspectSchema)(
			IAssetPackageByteSource&, const FPackagePath&,
			const FReflectionSchemaCatalog&, FPackageSchemaInspection&,
			FPackageSchemaReadStats*, bool,
			const FPackageReadCancellationCheck&)
			-> std::expected<void, Durin::UnifiedError::FError> = nullptr;
		auto (*Load)(
			const FAssetPackageReadContext&, DPackage*&, FAssetLoadReport*,
			const std::function<std::expected<void, Durin::UnifiedError::FError>(DPackage*)>&,
			const std::function<void(DPackage*)>&) -> std::expected<void, Durin::UnifiedError::FError> = nullptr;
		// Encoding callbacks return a validated closure and leave output unchanged on failure.
		auto (*Write)(DPackage*, FAssetPackageEncodedClosure&, EDefaultDeltaMode,
			const FAssetPackageCaptureOptions&) -> ObjectPackage::FPackageWriterResult = nullptr;
		auto (*RewriteReferences)(
			const FAssetPackageReadContext&, std::span<const FAssetPackageReferenceMapping>,
			uint64, FAssetPackageEncodedClosure&) -> ObjectPackage::FPackageWriterResult = nullptr;
		auto (*Relocate)(
			const FAssetPackageReadContext&, const FPackagePath&, FAssetPackageEncodedClosure&)
			-> ObjectPackage::FPackageWriterResult = nullptr;
		auto (*WriteRedirector)(
			const FPackagePath&, std::span<const FAssetRedirectorWriteMapping>,
			FAssetPackageEncodedClosure&)
			-> ObjectPackage::FPackageWriterResult = nullptr;
	};

	auto EncodeAssetPackageClosure(DPackage* Package, FAssetPackageEncodedClosure& OutClosure,
		const FAssetPackageCaptureOptions& Options) -> ObjectPackage::FPackageWriterResult;

	ENGINE_API auto FindAssetPackageReader(
		uint32 FormatVersion) -> const FAssetPackageCodec*;
	ENGINE_API auto FindAssetPackageWriter(
		uint32 FormatVersion) -> const FAssetPackageCodec*;
	ENGINE_API auto ResolveAssetPackageReader(
		FByteView Bytes, const FAssetPackageCodec*& OutCodec,
		uint32* OutFormatVersion = nullptr,
		uint64 PhysicalFileBytes = 0) -> std::expected<void, Durin::UnifiedError::FError>;
	auto ResolveAssetPackageReader(
		IAssetPackageByteSource& Source, const FAssetPackageCodec*& OutCodec,
		uint32* OutFormatVersion = nullptr,
		const FPackageReadCancellationCheck& IsCancelled = {},
		bool* OutUnsupportedFormat = nullptr) -> std::expected<void, Durin::UnifiedError::FError>;
	auto ValidateAssetPackageCodecPolicy(std::string& OutError) -> bool;
	ENGINE_API auto ValidateAssetPackageCodecTable(
		std::span<const FAssetPackageCodec> Codecs, std::string& OutError) -> bool;
}
