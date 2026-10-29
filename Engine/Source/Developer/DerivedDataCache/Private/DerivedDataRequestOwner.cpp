#include "DerivedDataRequestOwner.h"
#include "RequestOwnerPrivate.h"
#include "BuildSchedulerPrivate.h"
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
		class FOwnerState final : public IRequestOwnerInternal, public std::enable_shared_from_this<FOwnerState>
		{
		public:
			mutable std::mutex Mutex;
			bool Canceled = false;
			ERequestPriority Priority;
			struct FEntry { std::shared_ptr<IRequest> Request; std::shared_ptr<FOwnerTask> Dispatch; };
			std::unordered_map<IRequest*, FEntry> Requests;
			std::vector<Tasks::FTask> NativeTasks;
			std::vector<std::shared_ptr<const FRequestDependency>> Dependencies;
			uint64 DependencyRevision = 0;
			auto TrackTask(Tasks::FTask Task) -> void
			{
				if (!Task.IsValid()) return;
				std::lock_guard Lock(Mutex); PruneNativeTasks(); NativeTasks.push_back(std::move(Task)); ++DependencyRevision;
			}
			auto TrackDependency(FRequestDependency Value) -> void
			{
				require(Value.IsFinished && Value.Wait && Value.RequestCancel);
				auto Dependency = std::make_shared<const FRequestDependency>(std::move(Value));
				bool Cancel;
				{ std::lock_guard Lock(Mutex); Dependencies.push_back(Dependency); ++DependencyRevision; Cancel = Canceled; }
				if (Cancel) Dependency->RequestCancel();
			}
			explicit FOwnerState(ERequestPriority Priority) : Priority(Priority) {}
			auto Begin(std::shared_ptr<IRequest> Request) -> std::shared_ptr<IRequestOwnerInternal> override
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
						Tasks::FTask Native;
						{
							std::lock_guard DispatchLock(Retired.Dispatch->Mutex);
							if (Retired.Dispatch->Scheduled) Native = Private::GetNativeTask(*Retired.Dispatch->Scheduled);
						}
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
				std::vector<std::shared_ptr<const FRequestDependency>> External;
				{ std::lock_guard Lock(Mutex); Canceled = true; External = Dependencies; for (const auto& [Key, Request] : Requests) Snapshot.push_back(Request.Request); }
				for (const auto& Request : Snapshot) Request->RequestCancel();
				for (const auto& Dependency : External) Dependency->RequestCancel();
			}
			auto Cancel() -> void override
			{
				requiref(!IsInCallback(), "Request owners must be drained outside their callbacks; use RequestCancel.");
				RequestCancel();
				requiref(Wait() == ERequestWaitResult::Completed, "Request owner cancellation must wait for request end.");
			}
			auto IsCanceled() const -> bool override { std::lock_guard Lock(Mutex); return Canceled; }
			auto PruneNativeTasks() -> void
			{
				std::erase_if(NativeTasks, [](const auto& Task) { return Task.IsCompleted(); });
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
					if (Request && It == Requests.end()) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::DispatchRejected, "Request already completed."});
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
				std::vector<std::shared_ptr<const FRequestDependency>> External;
				uint64 Revision;
				{
					std::lock_guard Lock(Mutex);
					if (!Requests.empty() || !std::ranges::all_of(NativeTasks, [](const auto& Task) { return Task.IsCompleted(); })) return false;
					External = Dependencies; Revision = DependencyRevision;
				}
				if (!std::ranges::all_of(External, [](const auto& Dependency) { return Dependency->IsFinished(); })) return false;
				std::lock_guard Lock(Mutex);
				return Requests.empty() && Revision == DependencyRevision && std::ranges::all_of(NativeTasks, [](const auto& Task) { return Task.IsCompleted(); });
			}
			auto Wait() const -> ERequestWaitResult override
			{
				if (IsInCallback()) return ERequestWaitResult::WouldBlock;
				for (;;)
				{
					std::shared_ptr<IRequest> Request;
					std::vector<Tasks::FTask> Native;
					std::vector<std::shared_ptr<const FRequestDependency>> External;
					{
						std::lock_guard Lock(Mutex);
						if (Requests.empty()) { Native = NativeTasks; External = Dependencies; }
						else Request = Requests.begin()->second.Request;
					}
					if (Request) { if (Request->Wait() != ERequestWaitResult::Completed) return ERequestWaitResult::WouldBlock; }
					else
					{
						for (const auto& Task : Native) if (WaitTask(Task).WaitStatus != ETaskWaitStatus::Completed) return ERequestWaitResult::WouldBlock;
						for (const auto& Dependency : External)
						{
							FRequestOwnerScope Scope(const_cast<FOwnerState*>(this));
							if (Dependency->Wait() != ERequestWaitResult::Completed) return ERequestWaitResult::WouldBlock;
						}
						if (Poll()) return ERequestWaitResult::Completed;
					}
				}
			}
		};
	}
	struct FRequestBarrier::FState
	{
		struct FBarrierRequest final : Private::FRequestCompletion
		{
			// Cancellation cannot release a submission's unpublished handles.
			auto RequestCancel() -> bool override { return false; }
		};
		static auto Begin(IRequestOwner& Owner) -> std::shared_ptr<FBarrierRequest>
		{
			auto Request = std::make_shared<FBarrierRequest>();
			Request->Group = Private::BeginRequest(Owner, Request);
			Request->DispatchReturned();
			return Request;
		}
		std::shared_ptr<FBarrierRequest> Request;
		Private::FRequestOwnerScope Scope;
		explicit FState(IRequestOwner& Owner) : Request(Begin(Owner)), Scope(Request->Group.get(), Request.get()) {}
		~FState() { Request->Complete(); }
	};
	FRequestBarrier::FRequestBarrier(IRequestOwner& Owner) : State(std::make_unique<FState>(Owner)) {}
	FRequestBarrier::~FRequestBarrier() = default;

	FRequestOwner::FRequestOwner(ERequestPriority Priority)
		: IRequestOwner(std::make_shared<Private::FOwnerState>(Priority))
	{
	}
	FRequestOwner::~FRequestOwner() { Cancel(); }
	auto IRequestOwner::Begin(std::shared_ptr<IRequest> Request) -> void { State->Begin(std::move(Request)); }
	auto IRequestOwner::End(IRequest* Request) -> void { State->End(Request); }
	auto IRequestOwner::Cancel() -> void { State->Cancel(); }
	auto IRequestOwner::RequestCancel() -> void { State->RequestCancel(); }
	auto IRequestOwner::IsCanceled() const -> bool { return State->IsCanceled(); }
	auto IRequestOwner::GetPriority() const -> ERequestPriority { return State->GetPriority(); }
	auto IRequestOwner::SetPriority(ERequestPriority Value) -> void { State->SetPriority(Value); }
	auto IRequestOwner::Poll() const -> bool { return State->Poll(); }
	auto IRequestOwner::Wait() const -> ERequestWaitResult { return State->Wait(); }
	auto IRequestOwner::LaunchTask(std::string_view Name, std::function<void()> Work) -> void
	{
		FRequestBarrier Submission(*this);
		auto Fallback = Work;
		auto Result = State->LaunchTask(nullptr, *CreateTaskBuildScheduler(), Name, Work, Work);
		if (!Result)
		{
			try
			{
				if (Fallback) Fallback();
			}
			catch (...)
			{
			}
		}
	}
	auto Private::FOwnerAccess::TrackTask(FRequestOwner& Owner, Tasks::FTask Task) -> void
	{
		std::static_pointer_cast<Private::FOwnerState>(Get(Owner))->TrackTask(std::move(Task));
	}
	auto Private::FOwnerAccess::TrackDependency(FRequestOwner& Owner, Private::FRequestDependency Dependency) -> void
	{
		std::static_pointer_cast<Private::FOwnerState>(Get(Owner))->TrackDependency(std::move(Dependency));
	}
	auto Private::CreateRequestOwnerState() -> std::shared_ptr<Private::IRequestOwnerInternal>
	{
		return std::make_shared<Private::FOwnerState>(ERequestPriority::Normal);
	}
}
