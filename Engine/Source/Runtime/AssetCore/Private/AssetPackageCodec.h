#pragma once

#include "Asset/Compatibility.h"
#include "Asset/Mutation.h"
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
		uint32 FormatVersion = 0;
	};

	auto ReadAssetPackagePreamble(
		std::span<const std::byte> Bytes, FAssetPackagePreamble& OutPreamble) -> FAssetResult;
	auto FindAssetPackageReader(uint32 FormatVersion) -> const FAssetPackageCodec*;
	auto FindAssetPackageWriter(uint32 FormatVersion) -> const FAssetPackageCodec*;
	auto ResolveAssetPackageReader(
		std::span<const std::byte> Bytes, const FAssetPackageCodec*& OutCodec,
		FAssetPackagePreamble* OutPreamble = nullptr) -> FAssetResult;
	auto ValidateAssetPackageCodecPolicy(std::string& OutError) -> bool;
}
