#pragma once

#include "AssetCompatibility.h"
#include "AssetSystemInternal.h"
#include "DObject/DefaultDeltaPlan.h"

namespace Durin::Asset::Private
{
	// Defines the complete engine-owned capability set for one immutable package format.
	struct FAssetPackageCodec
	{
		std::string_view CodecId;
		uint32 FormatVersion = 0;
		bool bCanRead = false;
		bool bCanWrite = false;
		bool bCanMutate = false;

		auto (*ReadHeader)(std::span<const uint8>, uint64, FAssetPackageHeader&)
			-> FAssetResult = nullptr;
		auto (*Validate)(std::span<const uint8>) -> FAssetResult = nullptr;
		auto (*Inspect)(std::span<const uint8>, FAssetPackageInspection&) -> FAssetResult = nullptr;
		auto (*ExtractReferences)(
			std::span<const uint8>, const FAssetPath&, std::vector<FAssetReferenceEdge>&)
			-> FAssetResult = nullptr;
		auto (*ProbeCompatibility)(
			std::span<const uint8>, const FAssetPath&,
			const FReflectionCompatibilityCatalog&, FAssetPackageCompatibilityRecord&,
			FAssetCompatibilityProbeStats*) -> FAssetResult = nullptr;
		auto (*Load)(
			std::span<const uint8>, const FAssetPath&, DPackage*&, FAssetLoadReport*,
			const std::function<FAssetResult(DPackage*)>&,
			const std::function<void(DPackage*)>&) -> FAssetResult = nullptr;
		auto (*Write)(DPackage*, std::vector<uint8>&, EDefaultDeltaMode,
			const FAssetPackageSerializationOptions&) -> FAssetResult = nullptr;
		auto (*RewriteReferences)(
			std::span<const uint8>, std::span<const FAssetRedirectorFixupMapping>,
			uint64, std::vector<uint8>&) -> FAssetResult = nullptr;
		auto (*Relocate)(
			std::span<const uint8>, const FAssetPath&, std::vector<uint8>&)
			-> FAssetResult = nullptr;
		auto (*WriteRedirector)(
			const FAssetPath&, const FAssetPath&, std::vector<uint8>&)
			-> FAssetResult = nullptr;
	};

	struct FAssetPackagePreamble
	{
		uint32 FormatVersion = 0;
	};

	auto ReadAssetPackagePreamble(
		std::span<const uint8> Bytes, FAssetPackagePreamble& OutPreamble) -> FAssetResult;
	auto FindAssetPackageReader(uint32 FormatVersion) -> const FAssetPackageCodec*;
	auto FindAssetPackageWriter(uint32 FormatVersion) -> const FAssetPackageCodec*;
	auto ResolveAssetPackageReader(
		std::span<const uint8> Bytes, const FAssetPackageCodec*& OutCodec,
		FAssetPackagePreamble* OutPreamble = nullptr) -> FAssetResult;
	auto ValidateAssetPackageCodecPolicy(std::string& OutError) -> bool;
}
