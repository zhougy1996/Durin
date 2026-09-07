#pragma once

#include "AssetSubsystemFwd.h"
#include "Asset/AssetDefinitions.h"
#include "Asset/PackageSerialization.h"
#include "Asset/PackageResource.h"
#include "Asset/Load.h"
#include "DObject/DefaultDeltaPlan.h"
#include "DObject/PackageFormat.h"

namespace Durin::AssetPrivate
{
	enum class ELinkerLoadPhase : uint8
	{
		CreateSkeleton,
		ResolveDependency,
		ApplyValues,
		RestoreLedger,
		PostLoad,
		Publish,
	};

	struct FLinkerLoadOptions
	{
		std::function<bool(ELinkerLoadPhase, uint64)> ShouldFail;
		std::function<FAssetResult(DPackage*)> OnSkeletonReady;
		std::function<void(DPackage*)> OnSkeletonRollback;
		uint32 SourceFormatVersion = ObjectPackage::DastV9FormatVersion;
		bool bCooked = false;
		FArchiveTarget Target;
	};

	// A validated saved closure; the caller owns path admission and edit/save leases.
	struct FPackageGraphSource
	{
		FPackagePath PackagePath;
		FPreparedPackageResource Storage;
	};

	// Deserialization admission is exact-class and includes constructors, default
	// inners, native serializers and struct migration callbacks: these must not
	// mutate live state or pump callbacks during construction. No PostLoad runs.
	struct FPackageGraphPrepareOptions
	{
		std::vector<const DClass*> AdmittedClasses;
		uint64 MaximumPackages = 16;
		uint64 MaximumObjects = 65536;
		// Optional caller-owned scope for ordinary external dependency loads (and
		// their PostLoad). The caller admits that closure separately, retains the
		// scope on failure and releases it after dropping candidate references.
		// Scoped loads require resident targets to prevent recursive publication of them.
		FAssetPackageLoadScope* DependencyLoadScope = nullptr;
		std::function<bool()> IsCancelled;
		std::function<bool(uint64, ELinkerLoadPhase, uint64)> ShouldFail;
	};

	// ValuesPrepared does not mean runtime products or reference publication are ready.
	enum class EPackageGraphPrepareStatus : uint8
	{
		ValuesPrepared,
		Unsupported,
		InvalidClosure,
		MissingDependency,
		BudgetExceeded,
		Stale,
		Cancelled,
		Busy,
	};

	struct FPackageGraphPrepareResult
	{
		EPackageGraphPrepareStatus Status = EPackageGraphPrepareStatus::ValuesPrepared;
		FPackagePath PackagePath;
		std::string Message;
		explicit operator bool() const { return Status == EPackageGraphPrepareStatus::ValuesPrepared; }
	};

	class FPreparedPackageGraph;

	// GameThread only, failure preserves Out. All batch skeletons precede values;
	// external dependencies are pinned by exact identity. Missing dependencies require
	// an explicit caller-owned load scope; this seam never releases that scope.
	// Projection fences and active loads fail Busy; other reload admission is caller-owned.
	// Object limits include package/default-inner objects; byte budgets are caller-owned.
	ENGINE_API auto PreparePackageGraphs(std::span<const FPackageGraphSource> Sources,
		const FPackageGraphPrepareOptions& Options, std::vector<FPreparedPackageGraph>& Out)
		-> FPackageGraphPrepareResult;

	// Owns one unpublished deserialized graph and immutable storage. Destruction
	// marks only this private hierarchy as garbage; it never runs a global GC.
	// Ownership, access and destruction stay on GameThread.
	class FPreparedPackageGraph
	{
	public:
		ENGINE_API FPreparedPackageGraph();
		ENGINE_API ~FPreparedPackageGraph();
		ENGINE_API FPreparedPackageGraph(FPreparedPackageGraph&&) noexcept;
		ENGINE_API auto operator=(FPreparedPackageGraph&&) noexcept -> FPreparedPackageGraph&;
		FPreparedPackageGraph(const FPreparedPackageGraph&) = delete;
		auto operator=(const FPreparedPackageGraph&) -> FPreparedPackageGraph& = delete;
		ENGINE_API auto GetPackage() const -> DPackage*;
		ENGINE_API auto GetStorage() const -> const FPreparedPackageResource&;
		ENGINE_API auto GetReport() const -> const FAssetLoadReport&;

	private:
		struct FState;
		std::unique_ptr<FState> State;
		friend ENGINE_API auto PreparePackageGraphs(std::span<const FPackageGraphSource>,
			const FPackageGraphPrepareOptions&, std::vector<FPreparedPackageGraph>&)
			-> FPackageGraphPrepareResult;
	};

	auto CaptureLivePackageLinker(
		DPackage* Package,
		EDefaultDeltaMode DeltaMode,
		const FAssetPackageSerializationOptions& Options,
		ObjectPackage::FLinkerTables& OutLinker,
		std::string* OutError = nullptr) -> FAssetResult;

	auto ApplyLivePackageLinker(
		ObjectPackage::FLinkerTables Linker,
		const FPackagePath& PackagePath,
		DPackage*& OutPackage,
		FAssetLoadReport* OutReport,
		const FLinkerLoadOptions& Options = {},
		std::string* OutError = nullptr) -> FAssetResult;
}
