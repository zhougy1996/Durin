#pragma once

#include "EngineAPI.h"
#include "AssetSubsystemFwd.h"
#include "Asset/PackageSchema.h"
#include "AssetRegistry/Result.h"

namespace Durin
{
	enum class EDefaultDeltaMode : uint8;
}

namespace Durin::Asset::Private
{
	class IAssetPackageByteSource;

	struct FAssetPackageReadContext
	{
		std::span<const std::byte> PackageBytes;
		std::span<const std::byte> BulkBytes;
		FPackagePath PackagePath;
		uint64 PhysicalPackageBytes = 0;
		uint64 PhysicalBulkBytes = 0;
		bool bResourceBackedBulk = false;
		bool bCooked = false;
	};

	struct FAssetPackageEncodedClosure
	{
		std::vector<std::byte> PackageBytes;
		std::vector<std::byte> BulkBytes;
	};

	struct FAssetRedirectorWriteMapping
	{
		FTopLevelAssetPath Source;
		FObjectPath Destination;
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
			-> FAssetResult = nullptr;
		auto (*Validate)(const FAssetPackageReadContext&) -> FAssetResult = nullptr;
		auto (*Inspect)(const FAssetPackageReadContext&, FAssetPackageInspection&) -> FAssetResult = nullptr;
		auto (*ExtractReferences)(
			const FAssetPackageReadContext&, std::vector<FAssetReferenceEdge>&)
			-> FAssetResult = nullptr;
		auto (*InspectSchema)(
			IAssetPackageByteSource&, const FPackagePath&,
			const FReflectionSchemaCatalog&, FPackageSchemaInspection&,
			FPackageSchemaReadStats*, bool,
			const FPackageReadCancellationCheck&)
			-> FAssetResult = nullptr;
		auto (*Load)(
			const FAssetPackageReadContext&, DPackage*&, FAssetLoadReport*,
			const std::function<FAssetResult(DPackage*)>&,
			const std::function<void(DPackage*)>&) -> FAssetResult = nullptr;
		auto (*Write)(DPackage*, FAssetPackageEncodedClosure&, EDefaultDeltaMode,
			const FAssetPackageSerializationOptions&) -> FAssetResult = nullptr;
		auto (*RewriteReferences)(
			const FAssetPackageReadContext&, std::span<const FAssetRedirectorFixupMapping>,
			uint64, FAssetPackageEncodedClosure&) -> FAssetResult = nullptr;
		auto (*Relocate)(
			const FAssetPackageReadContext&, const FPackagePath&, FAssetPackageEncodedClosure&)
			-> FAssetResult = nullptr;
		auto (*WriteRedirector)(
			const FPackagePath&, std::span<const FAssetRedirectorWriteMapping>,
			FAssetPackageEncodedClosure&)
			-> FAssetResult = nullptr;
	};

	ENGINE_API auto FindAssetPackageReader(
		uint32 FormatVersion) -> const FAssetPackageCodec*;
	ENGINE_API auto FindAssetPackageWriter(
		uint32 FormatVersion) -> const FAssetPackageCodec*;
	ENGINE_API auto ResolveAssetPackageReader(
		std::span<const std::byte> Bytes, const FAssetPackageCodec*& OutCodec,
		uint32* OutFormatVersion = nullptr,
		uint64 PhysicalFileBytes = 0) -> FAssetResult;
	auto ResolveAssetPackageReader(
		IAssetPackageByteSource& Source, const FAssetPackageCodec*& OutCodec,
		uint32* OutFormatVersion = nullptr,
		const FPackageReadCancellationCheck& IsCancelled = {}) -> FAssetResult;
	auto ValidateAssetPackageCodecPolicy(std::string& OutError) -> bool;
	ENGINE_API auto ValidateAssetPackageCodecTable(
		std::span<const FAssetPackageCodec> Codecs, std::string& OutError) -> bool;
}
