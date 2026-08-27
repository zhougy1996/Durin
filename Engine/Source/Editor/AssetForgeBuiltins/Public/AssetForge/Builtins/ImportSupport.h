#pragma once

#include "Asset/PackageSerialization.h"
#include "AssetForgeBuiltinsAPI.h"
#include "DObject/AssetPath.h"
#include "Hash/XxHash.h"

namespace Durin::AssetForge
{
	enum class EImportDiagnosticSeverity : uint8
	{
		Info,
		Warning,
		Error
	};

	enum class EImportDiagnosticCategory : uint8
	{
		InvalidRequest,
		ProviderUnavailable,
		ProviderAmbiguous,
		ProviderFailure,
		CapabilityUnavailable,
		CandidateFailure,
		ValidationFailure,
		PublicationFailure,
		PersistenceFailure,
		RestoreFailure,
		InvalidSource,
		MissingDependency,
		UnsafeDependency,
		DuplicateSource,
		DependencyCycle,
		ResourceLimitExceeded,
		InvalidPlan,
		Collision,
		StalePlan,
		Canceled,
		AsyncFailure
	};

	struct FImportDiagnostic
	{
		EImportDiagnosticSeverity Severity = EImportDiagnosticSeverity::Error;
		EImportDiagnosticCategory Category = EImportDiagnosticCategory::InvalidRequest;
		// Providers may supply a stable identity. The framework derives one from
		// the stable context when this is empty before persistence.
		std::string Identity;
		std::string Phase;
		std::string SourceIdentity;
		std::string OutputIdentity;
		std::string Message;

		auto operator==(const FImportDiagnostic&) const -> bool = default;
	};

	ASSETFORGEBUILTINS_API auto GetImportDiagnosticIdentity(
		const FImportDiagnostic& Diagnostic) -> std::string;
	ASSETFORGEBUILTINS_API auto FinalizeImportDiagnostics(
		std::vector<FImportDiagnostic>& Diagnostics,
		std::string_view DefaultPhase,
		std::string_view DefaultSourceIdentity = "root",
		std::string_view DefaultOutputIdentity = "request") -> void;
	struct FSourceCaptureLimits
	{
		uint32 MaximumDependencyDepth = 32;
		uint32 MaximumSourceCount = 8'192;
		uint64 MaximumBytesPerSource = 2ull * 1024ull * 1024ull * 1024ull;
		uint64 MaximumAggregateBytes = 4ull * 1024ull * 1024ull * 1024ull;
		uint64 MaximumEmbeddedBytes = 2ull * 1024ull * 1024ull * 1024ull;
		uint64 RecognitionPrefixBytes = 64ull * 1024ull;
	};

	struct FSourceSnapshotEntry
	{
		std::string StableIdentity;
		std::string Role;
		std::string Filename;
		std::string DeclaringIdentity;
		FXxHash128 ContentHash{};
		uint64 ByteCount = 0;
		int64 LastWriteTime = 0;
		uint32 Depth = 0;
		bool bEmbedded = false;

		ASSETFORGEBUILTINS_API auto GetBytes() const -> std::span<const std::byte>;

		// Shared immutable storage permits several logical identities to reference
		// one physical capture without reopening or duplicating its bytes.
		std::shared_ptr<const std::vector<std::byte>> Bytes;
	};

	class ASSETFORGEBUILTINS_API FSourceSnapshot
	{
	public:
		auto GetSources() const -> std::span<const FSourceSnapshotEntry> { return Sources; }
		auto GetAggregateByteCount() const -> uint64 { return AggregateByteCount; }
		auto FindSource(std::string_view StableIdentity) const -> const FSourceSnapshotEntry*;

	private:
		std::vector<FSourceSnapshotEntry> Sources;
		uint64 AggregateByteCount = 0;

		friend class FSourceSnapshotBuilder;
	};

	struct FDependencyRequest
	{
		std::string DeclaringIdentity;
		std::string StableIdentity;
		std::string Role;
		std::string RelativePath;
		std::vector<std::byte> EmbeddedBytes;
		bool bOptional = false;

		auto IsEmbedded() const -> bool { return !EmbeddedBytes.empty(); }
	};

	class ASSETFORGEBUILTINS_API FDependencyRequestSink
	{
	public:
		auto AddRelative(
			std::string_view DeclaringIdentity,
			std::string_view StableIdentity,
			std::string_view Role,
			std::string_view RelativePath,
			bool bOptional = false) -> bool;
		auto AddEmbedded(
			std::string_view DeclaringIdentity,
			std::string_view StableIdentity,
			std::string_view Role,
			std::span<const std::byte> Bytes) -> bool;

	private:
		explicit FDependencyRequestSink(
			std::vector<FDependencyRequest>& InRequests,
			std::vector<FImportDiagnostic>& InDiagnostics,
			uint32 InMaximumRequests,
			uint64 InMaximumBytesPerSource,
			uint64 InMaximumEmbeddedBytes)
			: Requests(InRequests)
			, Diagnostics(InDiagnostics)
			, MaximumRequests(InMaximumRequests)
			, MaximumBytesPerSource(InMaximumBytesPerSource)
			, MaximumEmbeddedBytes(InMaximumEmbeddedBytes) {}

		std::vector<FDependencyRequest>& Requests;
		std::vector<FImportDiagnostic>& Diagnostics;
		uint32 MaximumRequests = 0;
		uint64 MaximumBytesPerSource = 0;
		uint64 MaximumEmbeddedBytes = 0;
		uint64 RequestedEmbeddedBytes = 0;

		friend class FSourceSnapshotBuilder;
	};

	enum class EImportOutputPolicy : uint8
	{
		Create,
		ReplaceWholeState
	};

	struct FImportOutputSummary
	{
		std::string StableIdentity;
		std::string Role;
		FAssetPath AssetPath;
		std::string AssetClassName;
		EImportOutputPolicy Policy = EImportOutputPolicy::Create;
		auto operator==(const FImportOutputSummary&) const -> bool = default;
	};

	// Serializes only final import preflight and authored-package publication.
	ASSETFORGEBUILTINS_API auto GetImportPublicationMutex() -> std::mutex&;

	class ASSETFORGEBUILTINS_API FSourceSnapshotBuilder
	{
	public:
		using FDependencyDiscovery = std::function<bool(
			std::span<const FSourceSnapshotEntry>,
			FDependencyRequestSink&,
			std::vector<FImportDiagnostic>&)>;
		explicit FSourceSnapshotBuilder(
			std::function<bool()> IsCancellationRequested = {},
			FSourceCaptureLimits InLimits = {});
		~FSourceSnapshotBuilder();
		FSourceSnapshotBuilder(FSourceSnapshotBuilder&&) noexcept;
		auto operator=(FSourceSnapshotBuilder&&) noexcept -> FSourceSnapshotBuilder&;
		FSourceSnapshotBuilder(const FSourceSnapshotBuilder&) = delete;
		auto operator=(const FSourceSnapshotBuilder&) -> FSourceSnapshotBuilder& = delete;

		auto CaptureRootFilename(
			std::string_view RootFilename,
			std::vector<FImportDiagnostic>& OutDiagnostics) -> bool;
		auto DiscoverSourceDependencies(
			const FDependencyDiscovery& Discovery,
			std::vector<FImportDiagnostic>& OutDiagnostics) -> bool;
		auto Freeze(
			std::vector<FImportDiagnostic>& OutDiagnostics) -> std::shared_ptr<const FSourceSnapshot>;
		auto GetCapturedSources() const -> std::span<const FSourceSnapshotEntry>;
		auto GetLimits() const -> const FSourceCaptureLimits&;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
