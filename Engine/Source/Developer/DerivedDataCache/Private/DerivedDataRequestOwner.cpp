#include "DerivedDataRequestOwner.h"
#include "RequestOwnerPrivate.h"
#include "Threading/Task.h"

namespace Durin::DerivedData
{
	thread_local Private::FRequestOwnerScope* Private::FRequestOwnerScope::Current = nullptr;
	namespace
	{
		class FOwnerTask final : public Private::FRequestCompletion, public IBuildScheduledWork
		{
		public:
			bool Started = false, Canceled = false;
			std::function<void()> Work, OnCanceled;
			auto Run(bool Cancel = false) -> bool
			{
				std::function<void()> Invoke, Discard;
				std::shared_ptr<IBuildScheduledWork> Pending;
				{
					std::lock_guard Lock(Mutex);
					if (Started) return false;
					Started = true;
					Canceled = Cancel;
					if (Cancel) Pending = Scheduled;
					Invoke = Cancel ? std::move(OnCanceled) : std::move(Work);
					Discard = Cancel ? std::move(Work) : std::move(OnCanceled);
				}
				if (Pending) Pending->RequestCancel();
				Complete([&] { Discard = {}; try { if (Invoke) Invoke(); } catch (...) {} Invoke = {}; }, nullptr);
				return true;
			}
			auto RequestCancel() -> bool override { return Run(true); }
			auto RequestCancel() const -> void override { const_cast<FOwnerTask*>(this)->RequestCancel(); }
			auto Wait() const -> ERequestWaitResult override { return const_cast<FOwnerTask*>(this)->Private::FRequestCompletion::Wait(); }
			auto Reject() -> void
			{
				std::function<void()> DiscardWork, DiscardCancellation;
				{ std::lock_guard Lock(Mutex); if (Started) return; Started = true; DiscardWork = std::move(Work); DiscardCancellation = std::move(OnCanceled); }
				Complete([&] { DiscardWork = {}; DiscardCancellation = {}; }, nullptr);
			}
		};
		struct FTaskDispatch
		{
			std::shared_ptr<FOwnerTask> Task;
			~FTaskDispatch() { Task->RequestCancel(); }
		};
	}
	namespace Private
	{
		class FOwnerState final : public IRequestOwner, public std::enable_shared_from_this<FOwnerState>
		{
		public:
			mutable std::mutex Mutex;
			bool Canceled = false;
			ERequestPriority Priority;
			struct FEntry { std::shared_ptr<IRequest> Request; std::shared_ptr<FOwnerTask> Dispatch; };
			std::unordered_map<IRequest*, FEntry> Requests;
			std::vector<FTaskHandle> NativeTasks;
			explicit FOwnerState(ERequestPriority Priority) : Priority(Priority) {}
			auto Begin(std::shared_ptr<IRequest> Request) -> std::shared_ptr<IRequestOwner> override
			{
				{ std::lock_guard Lock(Mutex); Requests.emplace(Request.get(), FEntry{Request, {}}); }
				return shared_from_this();
			}
			auto End(IRequest* Request) -> void override
			{
				FEntry Retired;
				{
					std::lock_guard Lock(Mutex);
					if (auto Node = Requests.extract(Request); !Node.empty()) Retired = std::move(Node.mapped());
					if (Retired.Dispatch)
					{
						FTaskHandle Native;
						{ std::lock_guard DispatchLock(Retired.Dispatch->Mutex); if (Retired.Dispatch->Scheduled) Native = Retired.Dispatch->Scheduled->GetTaskHandle(); }
						if (Native.IsValid())
						{
							PruneNativeTasks();
							NativeTasks.push_back(Native);
						}
					}
				}
				Retired.Request.reset();
				Retired.Dispatch.reset();
			}
			auto RequestCancel() -> void override
			{
				std::vector<std::shared_ptr<IRequest>> Snapshot;
				{ std::lock_guard Lock(Mutex); Canceled = true; for (const auto& [Key, Request] : Requests) Snapshot.push_back(Request.Request); }
				for (const auto& Request : Snapshot) Request->RequestCancel();
			}
			auto Cancel() -> void override
			{
				requiref(!IsInCallback(), "Request owners must be drained outside their callbacks; use RequestCancel.");
				RequestCancel();
				requiref(Wait() == ERequestWaitResult::Completed, "Request owner cancellation must join retirement.");
			}
			auto IsCanceled() const -> bool override { std::lock_guard Lock(Mutex); return Canceled; }
			auto PruneNativeTasks() -> void
			{
				std::erase_if(NativeTasks, [](const auto& Task) { return Task.IsComplete(); });
			}
			auto IsInCallback() const -> bool
			{
				std::lock_guard Lock(Mutex);
				for (auto* Scope = FRequestOwnerScope::Current; Scope; Scope = Scope->Previous)
					if (Scope->Owner == this || (Scope->Request && Requests.contains(Scope->Request))) return true;
				return false;
			}
			auto GetPriority() const -> ERequestPriority override { std::lock_guard Lock(Mutex); return Priority; }
			auto SetPriority(ERequestPriority Value) -> void override { std::lock_guard Lock(Mutex); Priority = Value; }
			auto LaunchTask(IRequest* Request, IBuildScheduler& Scheduler, std::string_view Name, std::function<void()> Work, std::function<void()> OnCanceled)
				-> std::expected<std::shared_ptr<IBuildScheduledWork>, FBuildAdmissionError> override
			{
				FBuildScheduleParams Params; Params.FunctionName = Name;
				auto Task = std::make_shared<FOwnerTask>(); Task->Group = shared_from_this();
				Task->Work = std::move(Work); Task->OnCanceled = std::move(OnCanceled);
				bool Cancel;
				{
					std::lock_guard Lock(Mutex);
					const auto It = Requests.find(Request);
					if (It == Requests.end()) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::DispatchRejected, "Request already completed."});
					Params.Priority = Priority; Cancel = Canceled;
					Requests.emplace(Task.get(), FEntry{Task, Task});
				}
				std::shared_ptr<FTaskDispatch> Dispatch;
				try
				{
					Dispatch = std::make_shared<FTaskDispatch>(); Dispatch->Task = Task;
					if (Cancel) { Task->RequestCancel(); Task->DispatchReturned(); return Task; }
					auto Accepted = Scheduler.Schedule(Params, [Dispatch] { auto Pinned = Dispatch; Pinned->Task->Run(); });
					if (!Accepted) Task->Reject();
					else {
						bool Canceled;
						{ std::lock_guard Lock(Task->Mutex); Task->Scheduled = *Accepted; Canceled = Task->Canceled; }
						if (Canceled && *Accepted) (*Accepted)->RequestCancel();
					}
					Task->DispatchReturned();
					if (!Accepted) return std::unexpected(std::move(Accepted.error()));
					return Task;
				}
				catch (...) { Task->Reject(); Task->DispatchReturned(); throw; }
			}
			auto Poll() const -> bool override
			{
				std::lock_guard Lock(Mutex);
				return Requests.empty() && std::ranges::all_of(NativeTasks, [](const auto& Task) { return Task.IsComplete(); });
			}
			auto Wait() const -> ERequestWaitResult override
			{
				if (IsInCallback()) return ERequestWaitResult::WouldBlock;
				for (;;)
				{
					std::shared_ptr<IRequest> Request;
					std::vector<FTaskHandle> Native;
					{
						std::lock_guard Lock(Mutex);
						if (Requests.empty()) Native = NativeTasks;
						else Request = Requests.begin()->second.Request;
					}
					if (Request) { if (Request->Wait() != ERequestWaitResult::Completed) return ERequestWaitResult::WouldBlock; }
					else
					{
						for (const auto& Task : Native) if (WaitTask(Task).WaitStatus != ETaskWaitStatus::Completed) return ERequestWaitResult::WouldBlock;
						return ERequestWaitResult::Completed;
					}
				}
			}
		};
	}
	FRequestOwner::FRequestOwner(ERequestPriority Priority) : State(std::make_shared<Private::FOwnerState>(Priority)) {}
	FRequestOwner::~FRequestOwner() { Cancel(); }
	auto FRequestOwner::Begin(std::shared_ptr<IRequest> Request) -> std::shared_ptr<IRequestOwner> { return State->Begin(std::move(Request)); }
	auto FRequestOwner::End(IRequest* Request) -> void { State->End(Request); }
	auto FRequestOwner::Cancel() -> void { State->Cancel(); }
	auto FRequestOwner::RequestCancel() -> void { State->RequestCancel(); }
	auto FRequestOwner::IsCanceled() const -> bool { return State->IsCanceled(); }
	auto FRequestOwner::GetPriority() const -> ERequestPriority { return State->GetPriority(); }
	auto FRequestOwner::SetPriority(ERequestPriority Priority) -> void { State->SetPriority(Priority); }
	auto FRequestOwner::Poll() const -> bool { return State->Poll(); }
	auto FRequestOwner::Wait() const -> ERequestWaitResult { return State->Wait(); }
	auto Private::CreateRequestOwnerState() -> std::shared_ptr<IRequestOwner> { return std::make_shared<FOwnerState>(ERequestPriority::Normal); }
	auto FRequestOwner::LaunchTask(IRequest* Request, IBuildScheduler& Scheduler, std::string_view Name, std::function<void()> Work, std::function<void()> OnCanceled)
		-> std::expected<std::shared_ptr<IBuildScheduledWork>, FBuildAdmissionError> { return State->LaunchTask(Request, Scheduler, Name, std::move(Work), std::move(OnCanceled)); }
}
