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
	struct FEditorToolSubmission
	{
		FEditorToolInvocationId Id;
		FToolFuture Future;
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
		// Shares the validated Future allocation and accounting; empty until publication.
		std::shared_ptr<const FToolResult> Result;
	};
	struct FEditorToolLimits
	{
		size_t MaxQueued = 256;
		size_t MaxCompleted = 1024;
		size_t MaxPending = 64;
		size_t MaxPendingBytes = 16 * 1024 * 1024;
		size_t MaxRetained = 1024;
		size_t MaxCatalogTools = 256;
		size_t MaxCatalogToolsets = 128;
		size_t MaxCatalogBytes = 16 * 1024 * 1024;
		size_t MaxQueuedBytes = 16 * 1024 * 1024;
		size_t MaxCompletedBytes = 32 * 1024 * 1024;
		size_t MaxDispatchPerTick = 16;
		std::chrono::milliseconds TickBudget{2};
		std::chrono::milliseconds ResultTTL{std::chrono::minutes(5)};
	};

	// Detached endpoint. Lookup, submission, retention and cancellation never access engine objects.
	class FEditorToolService final
	{
	public:
		DURINED_API ~FEditorToolService();
		FEditorToolService(const FEditorToolService&) = delete;
		auto operator=(const FEditorToolService&) -> FEditorToolService& = delete;
		[[nodiscard]] DURINED_API auto GetCatalog() const -> FEditorToolCatalog;
		[[nodiscard]] DURINED_API auto Submit(const FToolRequest& Request, EToolSource Source = EToolSource::Native,
			const FToolClientContext& Client = {})
			-> std::expected<FEditorToolInvocationId, FToolDiagnostic>;
		[[nodiscard]] DURINED_API auto GetInvocation(FEditorToolInvocationId Id) -> std::expected<FEditorToolInvocation, FToolDiagnostic>;
		[[nodiscard]] DURINED_API auto RetainInvocation(FEditorToolInvocationId Id) -> std::expected<FToolFuture, FToolDiagnostic>;
		[[nodiscard]] DURINED_API auto Cancel(FEditorToolInvocationId Id) -> FToolValidationResult;
		// Detached Python status/cancel only, active during the script pump. The
		// catalog identity is rechecked; callbacks may access owned job data only.
		using FPythonJobControl = std::function<FToolResult(const FToolRequest&, const FToolContext&)>;
		DURINED_API auto BindPythonJobControl(uint64 ToolsetIdentity, FPythonJobControl Control) -> void;
		DURINED_API auto GetWorkGate() const -> std::shared_ptr<const FSubsystemWorkGate>;
		// GameThread configuration, permitted only with no queued/running invocation.
		[[nodiscard]] DURINED_API auto Configure(const FEditorToolLimits& Limits) -> FToolValidationResult;
	private:
		explicit FEditorToolService(DEditorEngine& Editor, std::shared_ptr<const FSubsystemWorkGate> Gate);
		auto RefreshCatalog() -> void;
		auto CanDispatch() const -> bool;
		auto Call(const FToolRequest& Request, EToolSource Source, bool bAvailabilityOnly,
			FToolAvailability* Availability = nullptr, const FToolClientContext& Client = {},
			FToolCancellationToken Cancellation = {}) -> FToolResult;
		auto SubmitNow(const FToolRequest& Request, EToolSource Source,
			const FToolClientContext& Client, FToolCancellationToken Cancellation) -> std::expected<FEditorToolSubmission, FToolDiagnostic>;
		auto Drain() -> void;
		auto PumpScripts(const std::shared_ptr<FEditorToolService>& Endpoint) -> void;
		auto Close() noexcept -> void;
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
		friend class DEditorToolSubsystem;
	};

	// Exact synchronous binding only: returns nullptr outside this host's provider callback.
	[[nodiscard]] DURINED_API auto GetToolEditor(const FToolContext& Context) -> DEditorEngine*;
}
