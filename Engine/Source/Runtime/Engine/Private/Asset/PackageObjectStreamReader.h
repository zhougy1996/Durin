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
		uint32 SourceFormatVersion = AssetPackageV9FormatVersion;
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
		friend auto LoadDecodedAssetPackage(
			FDecodedPackage, const FPackagePath&, FLoadedAssetPackage&,
			FAssetLoadReport*, const FLiveLoadOptions&, FReaderDiagnostic*)
			-> FAssetResult;
	};

	// Applies an already validated, detached logical package without parsing an
	// Engine-owned package wire representation.
	auto LoadDecodedAssetPackage(
		FDecodedPackage Package,
		const FPackagePath& PackagePath,
		FLoadedAssetPackage& OutPackage,
		FAssetLoadReport* OutReport = nullptr,
		const FLiveLoadOptions& Options = {},
		FReaderDiagnostic* OutDiagnostic = nullptr) -> FAssetResult;

}
