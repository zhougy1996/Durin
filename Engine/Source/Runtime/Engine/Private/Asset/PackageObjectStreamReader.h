#pragma once

#include "AssetSubsystemFwd.h"
#include "AssetRegistry/Result.h"
#include "PackageObjectStreamWriter.h"

namespace Durin::Asset::PackageObjectStream
{
	enum class ELiveLoadPhase : uint8
	{
		CreateSkeleton,
		ResolveDependency,
		ApplyValues,
		RestoreLedger,
		PostLoad,
		Publish,
	};

	struct FLiveLoadOptions
	{
		// Test and higher-level transaction hook. Returning true fails before
		// the indexed operation without publishing the graph.
		std::function<bool(ELiveLoadPhase, uint64)> ShouldFail;
		// Optional high-level residency transaction hooks. The skeleton callback
		// runs after the complete object graph exists and before dependencies are
		// resolved, allowing dependency cycles to observe the in-flight package.
		std::function<FAssetResult(DPackage*)> OnSkeletonReady;
		std::function<void(DPackage*)> OnSkeletonRollback;
		uint32 SourceFormatVersion = AssetPackageObjectStreamVersion;
		bool bCooked = false;
		FArchiveTarget Target;
	};

	class FLoadedAssetPackage final
	{
	public:
		FLoadedAssetPackage() = default;
		ENGINE_API ~FLoadedAssetPackage();
		FLoadedAssetPackage(const FLoadedAssetPackage&) = delete;
		auto operator=(const FLoadedAssetPackage&) -> FLoadedAssetPackage& = delete;
		ENGINE_API FLoadedAssetPackage(FLoadedAssetPackage&& Other) noexcept;
		ENGINE_API auto operator=(FLoadedAssetPackage&& Other) noexcept -> FLoadedAssetPackage&;

		auto GetPackage() const -> DPackage* { return Package; }
		// Transfers the fully validated graph to version-neutral Engine Asset ownership.
		auto Release() -> DPackage* { return std::exchange(Package, nullptr); }
		explicit operator bool() const { return Package != nullptr; }
		ENGINE_API auto Reset() -> void;

	private:
		DPackage* Package = nullptr;
		explicit FLoadedAssetPackage(DPackage* InPackage) : Package(InPackage) {}
		friend ENGINE_API auto LoadAssetPackage(
			std::span<const std::byte>, const FAssetPath&, FLoadedAssetPackage&,
			FAssetLoadReport*, const FLiveLoadOptions&, const FReaderLimits&,
			FReaderDiagnostic*) -> FAssetResult;
	};

	// Explicit bytes-only object-stream live-load boundary. The returned handle owns the
	// standalone graph and is replaced only after dependencies, values, ledgers, and
	// PostLoad all succeed. It does not publish registry or ordinary-load policy.
	ENGINE_API auto LoadAssetPackage(
		std::span<const std::byte> Bytes,
		const FAssetPath& PackagePath,
		FLoadedAssetPackage& OutPackage,
		FAssetLoadReport* OutReport = nullptr,
		const FLiveLoadOptions& Options = {},
		const FReaderLimits& Limits = {},
		FReaderDiagnostic* OutDiagnostic = nullptr) -> FAssetResult;

	// Construct-free projection used by compatibility and reference tooling.
	ENGINE_API auto InspectPackage(
		std::span<const std::byte> Bytes,
		FAssetPackageInspection& OutInspection,
		const FReaderLimits& Limits = {},
		FReaderDiagnostic* OutDiagnostic = nullptr) -> FAssetResult;

	ENGINE_API auto RewriteReferences(
		std::span<const std::byte> Bytes,
		std::span<const FAssetRedirectorFixupMapping> Mappings,
		uint64 ExpectedRewriteCount,
		std::vector<std::byte>& OutBytes) -> FAssetResult;

	ENGINE_API auto RelocatePackage(
		std::span<const std::byte> Bytes,
		const FAssetPath& DestinationPath,
		std::vector<std::byte>& OutBytes) -> FAssetResult;

	ENGINE_API auto ProbeCompatibility(
		std::span<const std::byte> Bytes,
		const FAssetPath& PackagePath,
		const FReflectionCompatibilityCatalog& Catalog,
		FAssetPackageCompatibilityRecord& OutRecord,
		FAssetCompatibilityProbeStats* OutStats = nullptr,
		const FReaderLimits& Limits = {},
		FReaderDiagnostic* OutDiagnostic = nullptr) -> FAssetResult;

	// Compatibility evaluator for package codecs that already decoded the
	// canonical logical tables and value descriptors from non-contiguous ranges.
	auto ProbeDecodedCompatibility(
		FDecodedPackage Package,
		uint64 PhysicalPackageBytes,
		bool bPayloadValuesDecoded,
		const FAssetPath& PackagePath,
		const FReflectionCompatibilityCatalog& Catalog,
		FAssetPackageCompatibilityRecord& OutRecord,
		FReaderDiagnostic* OutDiagnostic = nullptr) -> FAssetResult;

	// Returns true only when a nested serialized value can contain an exact
	// versioned deprecated-property route. Root overrides need descriptors only.
	auto RequiresDecodedCompatibilityPayloadValues(
		const FDecodedPackage& Package,
		const FReflectionCompatibilityCatalog& Catalog) -> bool;
}
