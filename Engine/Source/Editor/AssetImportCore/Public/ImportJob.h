#pragma once

#include "AsyncImport.h"

namespace Durin::Asset
{
	// Provider-owned values may contain only detached, destructible data. The
	// operation registry destroys every value before releasing its provider lease.
	class IImportJobValue
	{
	public:
		virtual ~IImportJobValue() = default;
	};

	struct FImportJobWorkerContext
	{
		const FTaskCancellationToken& Cancellation;
		IImportProgressReporter& Progress;
	};

	class FImportJobEditorContext
	{
	public:
		virtual ~FImportJobEditorContext() = default;
		virtual auto IsCancellationRequested() const -> bool = 0;
		virtual auto EnterFinalization() -> bool = 0;
		virtual auto GetProgressReporter() -> IImportProgressReporter& = 0;
	};

	struct FImportJobWorkerStep
	{
		std::string Name;
		FTaskAttribution Attribution;
		uint64 EstimatedResultBytes = 64ull * 1'024ull;
		std::unique_ptr<IImportJobValue> Input;
	};

	struct FImportJobWorkerResult
	{
		bool bSucceeded = true;
		bool bCanceled = false;
		std::string Diagnostic;
		std::unique_ptr<IImportJobValue> Value;
	};

	struct FImportJobEditorAdvance
	{
		enum class EKind : uint8
		{
			Worker,
			Terminal
		};

		EKind Kind = EKind::Terminal;
		std::optional<FImportJobWorkerStep> Worker;
		std::optional<FImportOutcome> Outcome;

		static auto ContinueWith(FImportJobWorkerStep Step) -> FImportJobEditorAdvance
		{
			FImportJobEditorAdvance Advance;
			Advance.Kind = EKind::Worker;
			Advance.Worker.emplace(std::move(Step));
			return Advance;
		}

		static auto Complete(FImportOutcome InOutcome) -> FImportJobEditorAdvance
		{
			FImportJobEditorAdvance Advance;
			Advance.Outcome.emplace(std::move(InOutcome));
			return Advance;
		}

		auto IsValid() const -> bool
		{
			return Kind == EKind::Worker
				? Worker.has_value() && !Outcome.has_value() && !Worker->Name.empty()
				: Outcome.has_value() && Outcome->IsTerminal() && !Worker.has_value();
		}
	};

	class IImportJob
	{
	public:
		virtual ~IImportJob() = default;
		virtual auto GetProviderId() const -> std::string_view = 0;
		virtual auto RequiresLegacyProviderLease() const -> bool { return true; }
		virtual auto GetOwner() const -> const FImportOperationOwner& = 0;
		virtual auto GetLifetime() const -> EImportOperationLifetime = 0;
		virtual auto AdvanceOnEditor(
			FImportJobEditorContext& Context,
			std::unique_ptr<IImportJobValue> PreviousWorkerResult)
			-> FImportJobEditorAdvance = 0;
		virtual auto ExecuteWorkerStep(
			FImportJobWorkerContext& Context,
			std::unique_ptr<IImportJobValue> Input)
			-> FImportJobWorkerResult = 0;
		// Runs on the editor thread after a failed/canceled worker result so jobs
		// that captured editor-side provisional state can compensate before terminal.
		virtual auto CompensateWorkerFailureOnEditor(
			FImportJobEditorContext&,
			FImportJobWorkerResult Result) -> FImportOutcome
		{
			return {
				.State = Result.bCanceled
					? EImportOperationState::Canceled : EImportOperationState::Failed,
				.Diagnostic = std::move(Result.Diagnostic)};
		}
	};
}
