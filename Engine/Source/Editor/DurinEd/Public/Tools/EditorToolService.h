#pragma once

#include "CoreMinimal.h"
#include "Tools/Tool.h"
#include "DurinEdAPI.h"

namespace Durin
{
	class DEditorEngine;
	class DEditorToolSubsystem;
	class FSubsystemWorkGate;

	struct FEditorToolInvocationId
	{
		uint64 Session = 0;
		uint64 Sequence = 0;
		auto operator==(const FEditorToolInvocationId&) const -> bool = default;
	};
	struct FEditorToolsetCatalogEntry
	{
		FToolsetDescriptor Descriptor;
		uint64 ToolsetIdentity = 0;
	};
	struct FEditorToolCatalog
	{
		uint64 Revision = 0;
		uint64 FeatureRevision = 0;
		std::vector<FEditorToolsetCatalogEntry> Toolsets;
		std::vector<FToolDiagnostic> Diagnostics;
	};
	struct FEditorToolInvocation
	{
		EToolInvocationState State = EToolInvocationState::Queued;
		std::optional<FToolResult> Result;
	};
	struct FEditorToolLimits
	{
		size_t MaxQueued = 256;
		size_t MaxCompleted = 1024;
		size_t MaxCatalogTools = 256;
		size_t MaxCatalogToolsets = 128;
		size_t MaxCatalogBytes = 16 * 1024 * 1024;
		size_t MaxQueuedBytes = 16 * 1024 * 1024;
		size_t MaxCompletedBytes = 32 * 1024 * 1024;
		size_t MaxDispatchPerTick = 16;
		std::chrono::milliseconds TickBudget{2};
		std::chrono::milliseconds ResultTTL{std::chrono::minutes(5)};
	};

	// Detached endpoint. These four operations are thread-safe and never access engine objects.
	class FEditorToolService final
	{
	public:
		DURINED_API ~FEditorToolService();
		FEditorToolService(const FEditorToolService&) = delete;
		auto operator=(const FEditorToolService&) -> FEditorToolService& = delete;
		[[nodiscard]] DURINED_API auto GetCatalog() const -> FEditorToolCatalog;
		[[nodiscard]] DURINED_API auto Submit(const FToolRequest& Request, EToolSource Source = EToolSource::Native)
			-> std::expected<FEditorToolInvocationId, FToolDiagnostic>;
		[[nodiscard]] DURINED_API auto GetInvocation(FEditorToolInvocationId Id) -> std::expected<FEditorToolInvocation, FToolDiagnostic>;
		[[nodiscard]] DURINED_API auto Cancel(FEditorToolInvocationId Id) -> FToolValidationResult;
		// GameThread configuration, permitted only with no queued/running invocation.
		[[nodiscard]] DURINED_API auto Configure(const FEditorToolLimits& Limits) -> FToolValidationResult;
	private:
		explicit FEditorToolService(std::shared_ptr<const FSubsystemWorkGate> Gate);
		auto RefreshCatalog() -> void;
		auto CanDispatch(DEditorEngine& Editor) const -> bool;
		auto Call(DEditorEngine& Editor, const FToolRequest& Request, EToolSource Source, bool bAvailabilityOnly,
			FToolAvailability* Availability = nullptr) -> FToolResult;
		auto Drain(DEditorEngine& Editor) -> void;
		auto Close() noexcept -> void;
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
		friend class DEditorToolSubsystem;
	};

	// Exact synchronous binding only: returns nullptr outside this host's provider callback.
	[[nodiscard]] DURINED_API auto GetToolEditor(const FToolContext& Context) -> DEditorEngine*;
}
