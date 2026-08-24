#pragma once

#include "Asset/Compatibility.h"
#include "Asset/Mutation.h"
#include "Asset/PackageObjectStreamWriter.h"

#include <array>
#include <utility>

namespace Durin::Asset::PackageObjectStream
{
	inline constexpr uint32 MaximumSummaryBytes = 65'535;
	inline constexpr uint8 RequiredSectionCount = 5;
	inline constexpr uint64 MaximumHeaderBytes =
		13ull + MaximumSummaryBytes + RequiredSectionCount * 9ull;

	enum class EReaderFailure : uint8
	{
		None,
		TruncatedInput,
		InvalidPrimitive,
		InvalidUtf8,
		LimitExceeded,
		InvalidHeader,
		InvalidDirectory,
		InvalidTable,
		DescriptorCycle,
		InvalidTopology,
		InvalidValue,
		InvalidRetainedClosure,
		NonCanonical,
		UnknownClass,
		MissingDependency,
		TypeMismatch,
		ArchiveFailure,
		PostLoadFailure,
		PublicationFailure,
	};

	struct FReaderDiagnostic
	{
		EReaderFailure Failure = EReaderFailure::None;
		std::string LogicalPath;
		std::string Message;
		uint64 ByteOffset = 0;

		auto Reset() -> void { *this = {}; }
	};

	struct FReaderLimits
	{
		uint64 PackageBytes = MaximumPackageBytes;
		uint64 StringBytes = MaximumStringBytes;
		uint64 TableEntries = MaximumTableEntries;
		uint64 SchemaFields = MaximumSchemaFields;
		uint64 CustomVersions = MaximumCustomVersions;
		uint64 Dependencies = MaximumDependencies;
		uint64 ContainerElements = MaximumContainerElements;
		uint64 ByteValueBytes = MaximumByteValueBytes;
		uint32 ValueDepth = MaximumValueDepth;
	};

	enum class ESectionKind : uint8
	{
		Name = 0x01,
		Type = 0x02,
		Schema = 0x03,
		Object = 0x04,
		Value = 0x05,
	};

	struct FSectionDirectoryEntry
	{
		ESectionKind Kind = ESectionKind::Name;
		uint32 Offset = 0;
		uint32 Length = 0;

		auto operator==(const FSectionDirectoryEntry&) const -> bool = default;
	};

	struct FValidatedHeader
	{
		std::string AssetClass;
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		std::string RedirectDestination;
		std::vector<std::string> Dependencies;
		uint64 ObjectCount = 0;
		std::array<FSectionDirectoryEntry, RequiredSectionCount> Sections;
		uint64 BytesRead = 0;

		auto operator==(const FValidatedHeader&) const -> bool = default;
	};

	struct FDecodedType
	{
		ETypeOpcode Opcode = ETypeOpcode::Bool;
		std::string QualifiedName;
		uint64 Parameter = 0;
		std::vector<uint64> ChildTypeIds;
	};

	struct FDecodedField
	{
		std::string Name;
		uint64 TypeId = 0;
		uint64 AuthoredFlags = 0;
	};

	struct FDecodedSchema
	{
		std::string QualifiedName;
		std::vector<FDecodedField> Fields;
	};

	struct FDecodedObject
	{
		uint64 Id = 0;
		uint64 OuterId = 0;
		std::string Path;
		std::string ClassName;
		std::string ObjectName;
	};

	struct FDecodedOverride
	{
		uint64 SchemaId = 0;
		uint64 FieldId = 0;
		uint8 Provenance = 0;
		FValue Value;
		std::vector<std::byte> DescriptorClosure;
		std::vector<std::byte> RetainedPayload;
		uint64 PayloadOffset = 0;
		uint64 PayloadSize = 0;
	};

	struct FDecodedObjectValues
	{
		std::vector<FDecodedOverride> Overrides;
	};

	struct FDecodedPackage
	{
		FValidatedHeader Header;
		std::vector<std::string> Names;
		std::vector<FDecodedType> Types;
		std::vector<FCustomVersion> CustomVersions;
		std::vector<FDecodedSchema> Schemas;
		std::vector<FDecodedObject> Objects;
		std::vector<FDecodedObjectValues> ObjectValues;
	};

	// Parses only the bounded public summary and fixed five-entry directory.
	// PackageSize permits a bounded prefix to validate full-file section extents;
	// zero means the supplied span is the complete package. OutHeader is replaced
	// only after the complete header is valid.
	ASSETCORE_API auto ReadHeader(
		std::span<const std::byte> Bytes,
		FValidatedHeader& OutHeader,
		const FReaderLimits& Limits = {},
		FReaderDiagnostic* OutDiagnostic = nullptr,
		uint64 PackageSize = 0) -> bool;

	// Reconstructs the complete immutable logical package. Canonical validation
	// includes byte-for-byte re-emission through the production object-stream writer.
	ASSETCORE_API auto DecodePackage(
		std::span<const std::byte> Bytes,
		FDecodedPackage& OutPackage,
		const FReaderLimits& Limits = {},
		FReaderDiagnostic* OutDiagnostic = nullptr) -> bool;

	ASSETCORE_API auto ReencodePackage(
		const FDecodedPackage& Package,
		std::vector<std::byte>& OutBytes,
		FReaderDiagnostic* OutDiagnostic = nullptr) -> bool;

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
	};

	class ASSETCORE_API FLoadedAssetPackage final
	{
	public:
		FLoadedAssetPackage() = default;
		~FLoadedAssetPackage();
		FLoadedAssetPackage(const FLoadedAssetPackage&) = delete;
		auto operator=(const FLoadedAssetPackage&) -> FLoadedAssetPackage& = delete;
		FLoadedAssetPackage(FLoadedAssetPackage&& Other) noexcept;
		auto operator=(FLoadedAssetPackage&& Other) noexcept -> FLoadedAssetPackage&;

		auto GetPackage() const -> DPackage* { return Package; }
		// Transfers the fully validated graph to version-neutral AssetCore ownership.
		auto Release() -> DPackage* { return std::exchange(Package, nullptr); }
		explicit operator bool() const { return Package != nullptr; }
		auto Reset() -> void;

	private:
		DPackage* Package = nullptr;
		explicit FLoadedAssetPackage(DPackage* InPackage) : Package(InPackage) {}
		friend ASSETCORE_API auto LoadAssetPackage(
			std::span<const std::byte>, const FAssetPath&, FLoadedAssetPackage&,
			FAssetLoadReport*, const FLiveLoadOptions&, const FReaderLimits&,
			FReaderDiagnostic*) -> FAssetResult;
	};

	// Explicit bytes-only object-stream live-load boundary. The returned handle owns the
	// rooted graph and is replaced only after dependencies, values, ledgers, and
	// PostLoad all succeed. It does not publish registry or ordinary-load policy.
	ASSETCORE_API auto LoadAssetPackage(
		std::span<const std::byte> Bytes,
		const FAssetPath& PackagePath,
		FLoadedAssetPackage& OutPackage,
		FAssetLoadReport* OutReport = nullptr,
		const FLiveLoadOptions& Options = {},
		const FReaderLimits& Limits = {},
		FReaderDiagnostic* OutDiagnostic = nullptr) -> FAssetResult;

	// Construct-free projection used by compatibility and reference tooling.
	ASSETCORE_API auto InspectPackage(
		std::span<const std::byte> Bytes,
		FAssetPackageInspection& OutInspection,
		const FReaderLimits& Limits = {},
		FReaderDiagnostic* OutDiagnostic = nullptr) -> FAssetResult;

	ASSETCORE_API auto ExtractReferences(
		std::span<const std::byte> Bytes,
		const FAssetPath& SourcePackage,
		std::vector<FAssetReferenceEdge>& OutReferences,
		const FReaderLimits& Limits = {},
		FReaderDiagnostic* OutDiagnostic = nullptr) -> FAssetResult;

	ASSETCORE_API auto RewriteReferences(
		std::span<const std::byte> Bytes,
		std::span<const FAssetRedirectorFixupMapping> Mappings,
		uint64 ExpectedRewriteCount,
		std::vector<std::byte>& OutBytes) -> FAssetResult;

	ASSETCORE_API auto RelocatePackage(
		std::span<const std::byte> Bytes,
		const FAssetPath& DestinationPath,
		std::vector<std::byte>& OutBytes) -> FAssetResult;

	ASSETCORE_API auto ProbeCompatibility(
		std::span<const std::byte> Bytes,
		const FAssetPath& PackagePath,
		const FReflectionCompatibilityCatalog& Catalog,
		FAssetPackageCompatibilityRecord& OutRecord,
		FAssetCompatibilityProbeStats* OutStats = nullptr,
		const FReaderLimits& Limits = {},
		FReaderDiagnostic* OutDiagnostic = nullptr) -> FAssetResult;
}
