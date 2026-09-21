#pragma once

#include "EngineAPI.h"
#include "AssetSubsystemFwd.h"
#include "AssetPackageDependencyLoadPolicy.h"
#include "Asset/PackageSchema.h"
#include "Asset/AssetReadResult.h"
#include "DObject/PackageFormat.h"
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
		// Ordinary loading transfers candidate completion to its component owner.
		std::function<void(std::function<FAssetReadResult()>, std::function<void()>)> DeferCompletion;
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
			-> FAssetReadResult = nullptr;
		auto (*Validate)(const FAssetPackageReadContext&) -> FAssetReadResult = nullptr;
		auto (*Inspect)(const FAssetPackageReadContext&, FAssetPackageInspection&) -> FAssetReadResult = nullptr;
		auto (*ExtractReferences)(
			const FAssetPackageReadContext&, std::vector<FAssetReferenceEdge>&)
			-> FAssetReadResult = nullptr;
		auto (*InspectSchema)(
			IAssetPackageByteSource&, const FPackagePath&,
			const FReflectionSchemaCatalog&, FPackageSchemaInspection&,
			FPackageSchemaReadStats*, bool,
			const FPackageReadCancellationCheck&)
			-> FAssetReadResult = nullptr;
		auto (*Load)(
			const FAssetPackageReadContext&, DPackage*&, FAssetLoadReport*,
			const std::function<FAssetReadResult(DPackage*)>&,
			const std::function<void(DPackage*)>&) -> FAssetReadResult = nullptr;
		auto (*Write)(DPackage*, FAssetPackageEncodedClosure&, EDefaultDeltaMode,
			const FAssetPackageSerializationOptions&) -> ObjectPackage::FPackageWriterResult = nullptr;
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

	ENGINE_API auto FindAssetPackageReader(
		uint32 FormatVersion) -> const FAssetPackageCodec*;
	ENGINE_API auto FindAssetPackageWriter(
		uint32 FormatVersion) -> const FAssetPackageCodec*;
	ENGINE_API auto ResolveAssetPackageReader(
		FByteView Bytes, const FAssetPackageCodec*& OutCodec,
		uint32* OutFormatVersion = nullptr,
		uint64 PhysicalFileBytes = 0) -> FAssetReadResult;
	auto ResolveAssetPackageReader(
		IAssetPackageByteSource& Source, const FAssetPackageCodec*& OutCodec,
		uint32* OutFormatVersion = nullptr,
		const FPackageReadCancellationCheck& IsCancelled = {}) -> FAssetReadResult;
	auto ValidateAssetPackageCodecPolicy(std::string& OutError) -> bool;
	ENGINE_API auto ValidateAssetPackageCodecTable(
		std::span<const FAssetPackageCodec> Codecs, std::string& OutError) -> bool;
}
