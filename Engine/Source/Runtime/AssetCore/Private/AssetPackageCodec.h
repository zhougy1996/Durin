#pragma once

#include "Asset/Compatibility.h"
#include "Asset/Mutation.h"
#include "Asset/PackageVersionPolicy.h"
#include "DObject/DefaultDeltaPlan.h"
#include "Serialization/BinaryEnvelope.h"

namespace Durin::Asset::Private
{
	// Defines the complete engine-owned capability set for one immutable package format.
	struct FAssetPackageCodec
	{
		std::string_view CodecId;
		FGuid FormatId;
		uint32 FormatVersion = 0;
		bool bCanRead = false;
		bool bCanWrite = false;
		bool bCanMutate = false;

		auto (*ReadHeader)(std::span<const std::byte>, uint64, FAssetPackageHeader&)
			-> FAssetResult = nullptr;
		auto (*Validate)(std::span<const std::byte>) -> FAssetResult = nullptr;
		auto (*Inspect)(std::span<const std::byte>, FAssetPackageInspection&) -> FAssetResult = nullptr;
		auto (*ExtractReferences)(
			std::span<const std::byte>, const FAssetPath&, std::vector<FAssetReferenceEdge>&)
			-> FAssetResult = nullptr;
		auto (*ProbeCompatibility)(
			std::span<const std::byte>, const FAssetPath&,
			const FReflectionCompatibilityCatalog&, FAssetPackageCompatibilityRecord&,
			FAssetCompatibilityProbeStats*) -> FAssetResult = nullptr;
		auto (*Load)(
			std::span<const std::byte>, const FAssetPath&, DPackage*&, FAssetLoadReport*,
			const std::function<FAssetResult(DPackage*)>&,
			const std::function<void(DPackage*)>&) -> FAssetResult = nullptr;
		auto (*Write)(DPackage*, std::vector<std::byte>&, EDefaultDeltaMode,
			const FAssetPackageSerializationOptions&) -> FAssetResult = nullptr;
		auto (*RewriteReferences)(
			std::span<const std::byte>, std::span<const FAssetRedirectorFixupMapping>,
			uint64, std::vector<std::byte>&) -> FAssetResult = nullptr;
		auto (*Relocate)(
			std::span<const std::byte>, const FAssetPath&, std::vector<std::byte>&)
			-> FAssetResult = nullptr;
		auto (*WriteRedirector)(
			const FAssetPath&, const FAssetPath&, std::vector<std::byte>&)
			-> FAssetResult = nullptr;
	};

	struct FAssetPackagePreamble
	{
		FGuid FormatId;
		uint32 FormatVersion = 0;
		bool bUsesBinaryEnvelope = false;
	};

	ASSETCORE_API auto ReadAssetPackagePreamble(
		std::span<const std::byte> Bytes, FAssetPackagePreamble& OutPreamble,
		uint64 PhysicalFileBytes = 0) -> FAssetResult;
	ASSETCORE_API auto FindAssetPackageReader(
		const FGuid& FormatId, uint32 FormatVersion) -> const FAssetPackageCodec*;
	ASSETCORE_API auto FindAssetPackageWriter(
		const FGuid& FormatId, uint32 FormatVersion) -> const FAssetPackageCodec*;
	inline auto FindAssetPackageReader(uint32 FormatVersion) -> const FAssetPackageCodec*
	{
		return FindAssetPackageReader(DastBinaryFormatId, FormatVersion);
	}
	inline auto FindAssetPackageWriter(uint32 FormatVersion) -> const FAssetPackageCodec*
	{
		return FindAssetPackageWriter(DastBinaryFormatId, FormatVersion);
	}
	ASSETCORE_API auto ResolveAssetPackageReader(
		std::span<const std::byte> Bytes, const FAssetPackageCodec*& OutCodec,
		FAssetPackagePreamble* OutPreamble = nullptr,
		uint64 PhysicalFileBytes = 0) -> FAssetResult;
	auto ValidateAssetPackageCodecPolicy(std::string& OutError) -> bool;
	ASSETCORE_API auto ValidateAssetPackageCodecTable(
		std::span<const FAssetPackageCodec> Codecs, std::string& OutError) -> bool;
}
