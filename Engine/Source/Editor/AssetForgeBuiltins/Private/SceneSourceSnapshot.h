#pragma once

#include "AssetForge/Builtins/SceneImportTypes.h"
#include "Hash/XxHash.h"

namespace Durin::AssetForge
{
	auto FinalizeImportDiagnostics(
		std::vector<FImportDiagnostic>& Diagnostics,
		std::string_view DefaultPhase,
		std::string_view DefaultSourceIdentity = "root",
		std::string_view DefaultOutputIdentity = "request") -> void;

	// Bounds one Scene source closure before decoding or product construction.
	struct FSourceCaptureLimits
	{
		uint32 MaximumDependencyDepth = 32;
		uint32 MaximumSourceCount = 8'192;
		uint64 MaximumBytesPerSource = 2ull * 1024ull * 1024ull * 1024ull;
		uint64 MaximumAggregateBytes = 4ull * 1024ull * 1024ull * 1024ull;
	};

	// Owns one immutable physical source in the Scene capture closure.
	struct FSourceSnapshotEntry
	{
		std::string StableIdentity;
		std::string Filename;
		FXxHash128 ContentHash{};
		uint64 ByteCount = 0;
		uint32 Depth = 0;
		std::shared_ptr<const FByteArray> Bytes;

		auto GetBytes() const -> std::span<const std::byte>;
	};

	// Owns the finalized source closure consumed by Scene translation and building.
	class FSourceSnapshot
	{
	public:
		auto GetSources() const -> std::span<const FSourceSnapshotEntry> { return Sources; }
		auto FindSource(std::string_view StableIdentity) const -> const FSourceSnapshotEntry*;

	private:
		std::vector<FSourceSnapshotEntry> Sources;

		friend class FSourceSnapshotBuilder;
	};

	// Describes one dependency discovered from an already captured Scene source.
	struct FDependencyRequest
	{
		std::string DeclaringIdentity;
		std::string StableIdentity;
		std::string RelativePath;
		bool bOptional = false;
	};

	// Validates and collects bounded dependency requests during Scene discovery.
	class FDependencyRequestSink
	{
	public:
		auto AddRelative(
			std::string_view DeclaringIdentity,
			std::string_view StableIdentity,
			std::string_view RelativePath,
			bool bOptional = false) -> bool;

	private:
		explicit FDependencyRequestSink(
			std::vector<FDependencyRequest>& InRequests,
			std::vector<FImportDiagnostic>& InDiagnostics,
			uint32 InMaximumRequests)
			: Requests(InRequests)
			, Diagnostics(InDiagnostics)
			, MaximumRequests(InMaximumRequests) {}

		std::vector<FDependencyRequest>& Requests;
		std::vector<FImportDiagnostic>& Diagnostics;
		uint32 MaximumRequests = 0;

		friend class FSourceSnapshotBuilder;
	};

	// Captures and freezes one cancellation-aware immutable Scene source closure.
	class FSourceSnapshotBuilder
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

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
