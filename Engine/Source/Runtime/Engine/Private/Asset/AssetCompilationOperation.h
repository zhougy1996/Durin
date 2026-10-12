#pragma once

#include "CoreMinimal.h"
#include "DerivedDataRequestOwner.h"
#include "Threading/TaskComposition.h"

namespace Durin
{
	// Asset-layer bridge from DDC request retirement to a native task dependency.
	class FAssetCompilationOperation final : public DerivedData::IRequestOwner
	{
		class FState final : public DerivedData::IRequestOwner,
			public std::enable_shared_from_this<FState>
		{
			mutable std::mutex Mutex;
			DerivedData::FRequestOwner Owner;
			size_t ActiveRequests = 0, ActiveDispatches = 0;
			bool SubmissionClosed = false, Triggered = false;
			auto TryTriggerLocked() -> void
			{
				if (SubmissionClosed && !ActiveRequests && !ActiveDispatches && !Triggered)
				{
					Triggered = true;
					if (Completion) Completion->Trigger();
				}
			}
			class FTrackingScheduler final : public DerivedData::IBuildScheduler
			{
				FState& Operation;
				DerivedData::IBuildScheduler& Scheduler;
			public:
				FTrackingScheduler(FState& Operation, DerivedData::IBuildScheduler& Scheduler)
					: Operation(Operation), Scheduler(Scheduler) {}
				auto IsInline() const -> bool override { return Scheduler.IsInline(); }
				auto Schedule(const DerivedData::FBuildScheduleParams& Params, std::function<void()> Work)
					-> std::expected<std::shared_ptr<DerivedData::IBuildScheduledWork>, DerivedData::FBuildAdmissionError> override
				{
					auto Accepted = Scheduler.Schedule(Params, std::move(Work));
					if (Accepted && *Accepted)
					{
						const auto Native = (*Accepted)->GetTaskHandle();
						if (Native.IsValid())
						{
							std::lock_guard Lock(Operation.Mutex);
							Operation.Completion->AddPrerequisites(Tasks::FTaskCompletion(Native));
						}
					}
					return Accepted;
				}
			};
		public:
			std::optional<Tasks::FTaskEvent> Completion;
			explicit FState(DerivedData::ERequestPriority Priority)
				: Owner(Priority) {}
			auto GetCompletionEvent() -> Tasks::FTaskEvent&
			{
				std::lock_guard Lock(Mutex);
				if (!Completion)
				{
					Completion.emplace("AssetCompilation.DDC", false);
					if (Triggered) Completion->Trigger();
				}
				return *Completion;
			}

			// Close after the producer's final submission, including exceptional exits.
			auto CloseSubmission() -> void
			{
				std::lock_guard Lock(Mutex);
				SubmissionClosed = true;
				TryTriggerLocked();
			}
			auto Begin(std::shared_ptr<DerivedData::IRequest> Request) -> std::shared_ptr<DerivedData::IRequestOwner> override
			{
				GetCompletionEvent();
				std::lock_guard Lock(Mutex);
				requiref(!Triggered, "An asset operation cannot admit requests after completion.");
				Owner.Begin(std::move(Request));
				++ActiveRequests;
				return shared_from_this();
			}
			auto End(DerivedData::IRequest* Request) -> void override
			{
				Owner.End(Request);
				std::lock_guard Lock(Mutex);
				require(ActiveRequests > 0);
				--ActiveRequests;
				TryTriggerLocked();
			}
			auto RequestCancel() -> void override { Owner.RequestCancel(); }
			auto Cancel() -> void override { Owner.Cancel(); }
			auto IsCanceled() const -> bool override { return Owner.IsCanceled(); }
			auto GetPriority() const -> DerivedData::ERequestPriority override { return Owner.GetPriority(); }
			auto SetPriority(DerivedData::ERequestPriority Priority) -> void override { Owner.SetPriority(Priority); }
			auto Poll() const -> bool override { return Owner.Poll(); }
			auto Wait() const -> DerivedData::ERequestWaitResult override { return Owner.Wait(); }
			auto LaunchTask(DerivedData::IRequest* Request, DerivedData::IBuildScheduler& Scheduler, std::string_view Name,
				std::function<void()> Work, std::function<void()> OnCanceled)
				-> std::expected<std::shared_ptr<DerivedData::IBuildScheduledWork>, DerivedData::FBuildAdmissionError> override
			{
				{ std::lock_guard Lock(Mutex); ++ActiveDispatches; }
				struct FDispatchScope
				{
					FState& Operation;
					~FDispatchScope()
					{
						std::lock_guard Lock(Operation.Mutex);
						--Operation.ActiveDispatches;
						Operation.TryTriggerLocked();
					}
				} Dispatch{*this};
				FTrackingScheduler Tracking(*this, Scheduler);
				return Owner.LaunchTask(Request, Tracking, Name, std::move(Work), std::move(OnCanceled));
			}
		};
		std::shared_ptr<FState> State;
	public:
		explicit FAssetCompilationOperation(DerivedData::ERequestPriority Priority = DerivedData::ERequestPriority::Normal)
			: State(std::make_shared<FState>(Priority)) {}
		FAssetCompilationOperation(const FAssetCompilationOperation&) = delete;
		auto operator=(const FAssetCompilationOperation&) -> FAssetCompilationOperation& = delete;
		~FAssetCompilationOperation() override { Cancel(); CloseSubmission(); }
		// Unstarted queued asset records must not keep Core shutdown pending.
		auto GetCompletionEvent() -> Tasks::FTaskEvent& { return State->GetCompletionEvent(); }
		auto CloseSubmission() -> void { State->CloseSubmission(); }
		auto Begin(std::shared_ptr<DerivedData::IRequest> Request) -> std::shared_ptr<DerivedData::IRequestOwner> override { return State->Begin(std::move(Request)); }
		auto End(DerivedData::IRequest* Request) -> void override { State->End(Request); }
		auto RequestCancel() -> void override { State->RequestCancel(); }
		auto Cancel() -> void override { State->Cancel(); }
		auto IsCanceled() const -> bool override { return State->IsCanceled(); }
		auto GetPriority() const -> DerivedData::ERequestPriority override { return State->GetPriority(); }
		auto SetPriority(DerivedData::ERequestPriority Priority) -> void override { State->SetPriority(Priority); }
		auto Poll() const -> bool override { return State->Poll(); }
		auto Wait() const -> DerivedData::ERequestWaitResult override { return State->Wait(); }
		auto LaunchTask(DerivedData::IRequest* Request, DerivedData::IBuildScheduler& Scheduler, std::string_view Name,
			std::function<void()> Work, std::function<void()> OnCanceled)
			-> std::expected<std::shared_ptr<DerivedData::IBuildScheduledWork>, DerivedData::FBuildAdmissionError> override
		{ return State->LaunchTask(Request, Scheduler, Name, std::move(Work), std::move(OnCanceled)); }
	};
}
