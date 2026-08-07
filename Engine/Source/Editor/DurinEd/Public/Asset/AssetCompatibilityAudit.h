#pragma once

#include "DurinEdAPI.h"
#include "AssetCompatibility.h"
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

	DURINED_API auto MatchesAssetCompatibilityAuditFilter(
		const Asset::FAssetPackageCompatibilityRecord& Record,
		EAssetCompatibilityAuditFilter Filter) -> bool;
	DURINED_API auto CountAssetCompatibilityAuditRecords(
		std::span<const Asset::FAssetPackageCompatibilityRecord> Records)
		-> FAssetCompatibilityAuditCounts;
	DURINED_API auto FormatAssetCompatibilityAuditDiagnostics(
		const Asset::FAssetPackageCompatibilityRecord& Record) -> std::string;

	using FAssetCompatibilityProbe = std::function<Asset::FAssetPackageCompatibilityProbeResult(
		const Asset::FAssetPackageCompatibilityProbeInput&,
		const Asset::FReflectionCompatibilityCatalog&,
		const Asset::FAssetCompatibilityCancellationCheck&)>;

	// Game-thread-owned model for one explicit, request-scoped project audit. Workers
	// receive copied package inputs and a value-only reflection catalog.
	class FAssetCompatibilityAuditModel
	{
	public:
		DURINED_API explicit FAssetCompatibilityAuditModel(FAssetCompatibilityProbe InProbe = {});
		DURINED_API ~FAssetCompatibilityAuditModel();

		FAssetCompatibilityAuditModel(const FAssetCompatibilityAuditModel&) = delete;
		auto operator=(const FAssetCompatibilityAuditModel&) -> FAssetCompatibilityAuditModel& = delete;

		DURINED_API auto RunCurrentProjectAudit() -> bool;
		DURINED_API auto RunAudit(
			const std::unordered_map<FAssetPath, Asset::FAssetData>& Assets,
			Asset::FReflectionCompatibilityCatalog Catalog) -> bool;
		DURINED_API auto Cancel() -> bool;
		DURINED_API auto CancelAndDrain() -> void;
		DURINED_API auto ProjectChanged() -> void;
		DURINED_API auto Shutdown() -> void;

		// Drains worker notices and reconciles paths/fingerprints against the live
		// registry. It never scans the registry or reads package bytes.
		DURINED_API auto Tick(const std::unordered_map<FAssetPath, Asset::FAssetData>& Assets) -> void;
		DURINED_API auto GetPresentationRecords() const
			-> std::vector<Asset::FAssetPackageCompatibilityRecord>;
		DURINED_API auto FindRecord(const FAssetPath& Path) const
			-> const Asset::FAssetPackageCompatibilityRecord*;

		auto GetState() const -> EAssetCompatibilityAuditState { return State; }
		auto GetProgress() const -> FAssetCompatibilityAuditProgress { return Progress; }
		auto GetRequestSerial() const -> uint64 { return RequestSerial; }
		auto GetFailure() const -> const std::string& { return Failure; }
		auto GetWorkerTaskDiagnostics() const -> FTaskDiagnostics { return Task.GetDiagnostics(); }
		auto GetTerminalTaskDiagnostics() const -> FTaskDiagnostics { return TerminalTask.GetDiagnostics(); }

	private:
		struct FMailbox;
		struct FPublicationLifetime;
		auto DrainMailbox() -> void;
		auto Reconcile(const std::unordered_map<FAssetPath, Asset::FAssetData>& Assets) -> void;

		FAssetCompatibilityProbe Probe;
		std::shared_ptr<FMailbox> Mailbox;
		FTaskCancellationSource Cancellation;
		FTaskHandle Task;
		FTaskHandle TerminalTask;
		FTaskGenerationSource Generation;
		std::shared_ptr<FPublicationLifetime> PublicationLifetime;
		std::unordered_map<FAssetPath, Asset::FAssetPackageCompatibilityRecord> Records;
		FAssetCompatibilityAuditProgress Progress;
		EAssetCompatibilityAuditState State = EAssetCompatibilityAuditState::Idle;
		uint64 RequestSerial = 0;
		bool bAdmissionOpen = true;
		std::string Failure;
	};
}
