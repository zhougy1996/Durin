#pragma once

#include "MainFrameAPI.h"
#include "AssetRegistry/Catalog.h"
#include "AssetMaintenance/CompatibilityAudit.h"
#include "Threading/Task.h"

namespace Durin::Editor
{
	enum class EAssetCompatibilityAuditState : uint8
	{
		Idle,
		Running,
		Completed,
		Cancelled,
		Failed,
	};

	struct FAssetCompatibilityAuditProgress
	{
		uint64 Completed = 0;
		uint64 Total = 0;
	};

	enum class EAssetCompatibilityAuditFilter : uint8
	{
		All,
		Issues,
		Incompatible,
		Unsupported,
		Failed,
		Stale,
		NotChecked,
	};

	struct FAssetCompatibilityAuditCounts
	{
		uint64 Compatible = 0;
		uint64 Incompatible = 0;
		uint64 Unsupported = 0;
		uint64 Failed = 0;
		uint64 Stale = 0;
		uint64 NotChecked = 0;
	};

	MAINFRAME_API auto MatchesAssetCompatibilityAuditFilter(
		const FAssetPackageCompatibilityRecord& Record,
		EAssetCompatibilityAuditFilter Filter) -> bool;
	MAINFRAME_API auto MatchesAssetCompatibilityAuditSearch(
		const FAssetPackageCompatibilityRecord& Record,
		std::string_view SearchText) -> bool;
	MAINFRAME_API auto CountAssetCompatibilityAuditRecords(
		std::span<const FAssetPackageCompatibilityRecord> Records)
		-> FAssetCompatibilityAuditCounts;
	MAINFRAME_API auto FormatAssetCompatibilityAuditDiagnostics(
		const FAssetPackageCompatibilityRecord& Record) -> std::string;
	MAINFRAME_API auto FormatAssetCompatibilityAuditReport(
		std::span<const FAssetPackageCompatibilityRecord> Records) -> std::string;

	using FAssetCompatibilityProbe = FAssetCompatibilityProbeOperation;

	// Game-thread-owned model for one explicit, request-scoped project audit. Workers
	// receive copied package inputs and a value-only reflection catalog.
	class FAssetCompatibilityAuditModel
	{
	public:
		MAINFRAME_API explicit FAssetCompatibilityAuditModel(FAssetCompatibilityProbe InProbe = {});
		MAINFRAME_API ~FAssetCompatibilityAuditModel();

		FAssetCompatibilityAuditModel(const FAssetCompatibilityAuditModel&) = delete;
		auto operator=(const FAssetCompatibilityAuditModel&) -> FAssetCompatibilityAuditModel& = delete;

		MAINFRAME_API auto RunCurrentProjectAudit() -> bool;
		MAINFRAME_API auto RunAudit(
			const std::unordered_map<FPackagePath, FAssetData>& Assets,
			FReflectionCompatibilityCatalog Catalog) -> bool;
		MAINFRAME_API auto Cancel() -> bool;
		MAINFRAME_API auto CancelAndDrain() -> void;
		MAINFRAME_API auto ProjectChanged() -> void;
		MAINFRAME_API auto Shutdown() -> void;

		// Drains worker notices without scanning the registry or reading package bytes.
		MAINFRAME_API auto Tick() -> void;
		// Reconciles paths/fingerprints against a caller-owned catalog snapshot.
		// Callers should use the catalog revision to avoid redundant reconciliation.
		MAINFRAME_API auto ReconcileAssetCatalog(
			const std::unordered_map<FPackagePath, FAssetData>& Assets) -> void;
		// Convenience path for callers that already own a changed catalog snapshot.
		MAINFRAME_API auto Tick(const std::unordered_map<FPackagePath, FAssetData>& Assets) -> void;
		MAINFRAME_API auto GetPresentationRecords() const
			-> const std::vector<FAssetPackageCompatibilityRecord>&;
		MAINFRAME_API auto FindRecord(const FPackagePath& Path) const
			-> const FAssetPackageCompatibilityRecord*;

		auto GetState() const -> EAssetCompatibilityAuditState { return State; }
		auto GetProgress() const -> FAssetCompatibilityAuditProgress { return Progress; }
		auto GetRequestSerial() const -> uint64 { return RequestSerial; }
		auto GetPresentationRevision() const -> uint64 { return PresentationRevision; }
		auto GetFailure() const -> const std::string& { return Failure; }
		auto GetWorkerTaskDiagnostics() const -> FTaskDiagnostics { return Task.GetDiagnostics(); }
		auto GetTerminalTaskDiagnostics() const -> FTaskDiagnostics { return TerminalTask.GetDiagnostics(); }

	private:
		struct FMailbox;
		struct FPublicationLifetime;
		auto DrainMailbox() -> void;
		auto Reconcile(const std::unordered_map<FPackagePath, FAssetData>& Assets) -> void;
		auto InvalidatePresentation() -> void;

		FAssetCompatibilityProbe Probe;
		std::shared_ptr<FMailbox> Mailbox;
		FTaskCancellationSource Cancellation;
		FTaskHandle Task;
		FTaskHandle TerminalTask;
		FTaskGenerationSource Generation;
		std::shared_ptr<FPublicationLifetime> PublicationLifetime;
		std::unordered_map<FPackagePath, FAssetPackageCompatibilityRecord> Records;
		mutable std::vector<FAssetPackageCompatibilityRecord> PresentationRecords;
		mutable uint64 CachedPresentationRevision = std::numeric_limits<uint64>::max();
		uint64 PresentationRevision = 0;
		FAssetCompatibilityAuditProgress Progress;
		EAssetCompatibilityAuditState State = EAssetCompatibilityAuditState::Idle;
		uint64 RequestSerial = 0;
		bool bAdmissionOpen = true;
		std::string Failure;
	};
}
