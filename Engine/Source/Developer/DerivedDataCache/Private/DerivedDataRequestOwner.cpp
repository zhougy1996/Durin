#include "DerivedDataRequestOwner.h"
#include "RequestOwnerPrivate.h"
#include "Threading/Task.h"

namespace Durin::DerivedData
{
	thread_local Private::FRequestOwnerScope* Private::FRequestOwnerScope::Current = nullptr;
	namespace Private
	{
		class FRequestOwnerShared final : public IRequestOwner, public IRequest, public ICancelableRequest
		{
		public:
			explicit FRequestOwnerShared(ERequestPriority Value) : Priority(Value) { AddRef(); }
			auto Begin(IRequest* Request) -> void override
			{
				bool Replay;
				{ std::lock_guard Lock(Mutex);
					require(Request && (Barriers || !BeginExecuted));
					const auto [It, Inserted] = Requests.emplace(Request, TRefCountPtr<IRequest>(Request)); require(Inserted);
					AddRef(); BeginExecuted = true; Replay = PriorityChangedInBarrier;
				}
				if (Replay) try { ApplyPriority(*Request); } catch (...) { End(Request); throw; }
			}
			auto End(IRequest* Request) -> TRefCountPtr<IRequest> override
			{
				TRefCountPtr<IRequest> Retained;
				{ std::lock_guard Lock(Mutex); auto Node = Requests.extract(Request); require(!Node.empty()); Retained = std::move(Node.mapped()); }
				Changed.notify_all(); Release(); return Retained;
			}
			auto BeginBarrier(const FRequestBarrier& Barrier) -> void override
			{
				std::lock_guard Lock(Mutex); AddRef(); ++Barriers;
				if (Barrier.GetFlags() == ERequestBarrierFlags::Priority) ++PriorityBarriers;
			}
			auto EndBarrier(const FRequestBarrier& Barrier) -> void override
			{
				{ std::lock_guard Lock(Mutex); require(Barriers); --Barriers;
					if (Barrier.GetFlags() == ERequestBarrierFlags::Priority && --PriorityBarriers == 0) PriorityChangedInBarrier = false;
				}
				Changed.notify_all(); Release();
			}
			auto IsCanceled() const -> bool override { return Canceled.load(std::memory_order_relaxed); }
			auto GetPriority() const -> ERequestPriority override { return Priority.load(std::memory_order_relaxed); }
			auto SetPriority(ERequestPriority Value) -> void override
			{
				std::vector<TRefCountPtr<IRequest>> Snapshot;
				{ std::lock_guard Lock(Mutex); if (GetPriority() == Value) return;
					for (const auto& [Key, Request] : Requests) Snapshot.push_back(Request);
					Priority.store(Value, std::memory_order_relaxed); PriorityChangedInBarrier = PriorityBarriers != 0;
				}
				for (const auto& Request : Snapshot) ApplyPriority(*Request);
			}
			auto RequestCancel() -> void override
			{
				std::vector<TRefCountPtr<IRequest>> Snapshot;
				{ std::lock_guard Lock(Mutex); Canceled = true; for (const auto& [Key, Request] : Requests) Snapshot.push_back(Request); }
				for (const auto& Request : Snapshot) FOwnerAccess::RequestCancel(*Request);
			}
			auto Cancel() -> void override { Canceled = true; Drain(true); }
			auto Wait() -> void override { Drain(false); }
			auto Poll() const -> bool { std::lock_guard Lock(Mutex); return !Barriers && Requests.empty(); }
			auto KeepAlive() -> void { Retain = true; }
			auto Destroy() -> void { if (!Retain) Cancel(); Release(); }
		private:
			auto ApplyPriority(IRequest& Request) -> void
			{
				for (;;) { const auto Value = GetPriority(); Request.SetPriority(Value); if (Value == GetPriority()) return; }
			}
			auto Drain(bool Canceling) -> void
			{
				for (;;) {
					TRefCountPtr<IRequest> Request;
					{ std::unique_lock Lock(Mutex);
						if (!Requests.empty()) Request = Requests.begin()->second;
						else if (!Barriers) return;
						else {
							requiref(!FRequestBarrier::HasBarrierForOwnerOnCallingThread(*this), "Request owners must be waited outside their callbacks or barriers.");
							Changed.wait(Lock, [&] { return !Requests.empty() || !Barriers; }); continue;
						}
					}
					if (Canceling) Request->Cancel(); else Request->Wait();
				}
			}
			mutable std::mutex Mutex;
			std::condition_variable Changed;
			std::unordered_map<IRequest*, TRefCountPtr<IRequest>> Requests;
			uint32 Barriers = 0, PriorityBarriers = 0;
			bool BeginExecuted = false, PriorityChangedInBarrier = false;
			std::atomic<ERequestPriority> Priority;
			std::atomic_bool Canceled = false, Retain = false;
		};
	}
	namespace
	{
		auto ToSharedOwner(IRequestOwner& Owner) -> Private::FRequestOwnerShared& { return static_cast<Private::FRequestOwnerShared&>(Owner); }
		class FOwnerTask final : public Private::FRequestCompletion
		{
		public:
			bool Started = false;
			std::function<void()> Work, OnCanceled;
			auto Run(bool Canceled = false) -> void
			{
				std::function<void()> Invoke, Discard;
				{ std::lock_guard Lock(Mutex); if (Started) return; Started = true; Invoke = Canceled ? std::move(OnCanceled) : std::move(Work); Discard = Canceled ? std::move(Work) : std::move(OnCanceled); }
				Discard = {};
				Complete([&] { try { if (Invoke) Invoke(); } catch (...) {} Invoke = {}; });
			}
			auto RequestCancel() -> void override { Run(true); }
		};
		struct FTaskDispatch
		{
			TRefCountPtr<FOwnerTask> Task;
			~FTaskDispatch() { Task->RequestCancel(); }
		};
	}
	auto Private::FOwnerAccess::LaunchTask(IRequestOwner& Owner, IBuildScheduler& Scheduler, std::string_view Name,
		std::function<void()> Work, std::function<void()> OnCanceled) -> TRefCountPtr<IRequest>
	{
		FRequestBarrier Publication(Owner, ERequestBarrierFlags::Priority);
		auto Task = MakeRefCount<FOwnerTask>(); Task->Group = &Owner;
		Task->Work = std::move(Work); Task->OnCanceled = std::move(OnCanceled);
		Owner.Begin(Task);
		try {
			auto Dispatch = std::make_shared<FTaskDispatch>(); Dispatch->Task = Task;
			auto Scheduled = Scheduler.Schedule({Name, Owner.GetPriority()}, [Dispatch] { Dispatch->Task->Run(); });
			{ std::lock_guard Lock(Task->Mutex); Task->Scheduled = Scheduled; }
			Task->Changed.notify_all();
			if (Scheduled) for (;;) {
				const auto Priority = Owner.GetPriority(); Scheduled->SetPriority(Priority);
				if (Priority == Owner.GetPriority()) break;
			}
			return TRefCountPtr<IRequest>(Task);
		} catch (...) { Task->RequestCancel(); Task->Wait(); throw; }
	}
	static thread_local FRequestBarrier* RequestBarriersByThread = nullptr;
	FRequestBarrier::FRequestBarrier(IRequestOwner& Owner, ERequestBarrierFlags Flags)
		: Owner(Owner), NextOnThread(RequestBarriersByThread), Flags(Flags)
	{ RequestBarriersByThread = this; Owner.BeginBarrier(*this); }
	FRequestBarrier::~FRequestBarrier()
	{ Owner.EndBarrier(*this); require(RequestBarriersByThread == this); RequestBarriersByThread = NextOnThread; }
	auto FRequestBarrier::HasBarrierForOwnerOnCallingThread(IRequestOwner& Owner) -> bool
	{ for (auto* It = RequestBarriersByThread; It; It = It->NextOnThread) if (&It->Owner == &Owner) return true; return false; }
	IRequestOwner::~IRequestOwner() = default;
	auto FRequestOwner::FDeleteOwner::operator()(IRequestOwner* Owner) const -> void { ToSharedOwner(*Owner).Destroy(); }
	FRequestOwner::FRequestOwner(ERequestPriority Priority) : Owner(new Private::FRequestOwnerShared(Priority)) {}
	FRequestOwner::~FRequestOwner() = default;
	FRequestOwner::FRequestOwner(FRequestOwner&& Other) noexcept = default;
	auto FRequestOwner::operator=(FRequestOwner&& Other) noexcept -> FRequestOwner& = default;
	auto FRequestOwner::KeepAlive() -> void { ToSharedOwner(*Owner).KeepAlive(); }
	FRequestOwner::operator IRequestOwner&() const { require(Owner); return *Owner; }
	FRequestOwner::operator IRequest*() const { require(Owner); return &ToSharedOwner(*Owner); }
	auto FRequestOwner::Cancel() -> void { ToSharedOwner(*Owner).Cancel(); }
	auto FRequestOwner::Wait() const -> void { ToSharedOwner(*Owner).Wait(); }
	auto FRequestOwner::Poll() const -> bool { return ToSharedOwner(*Owner).Poll(); }
	auto FRequestOwner::SetPriority(ERequestPriority Value) -> void { ToSharedOwner(*Owner).SetPriority(Value); }
	auto FRequestOwner::GetPriority() const -> ERequestPriority { return Owner->GetPriority(); }
	auto FRequestOwner::IsCanceled() const -> bool { return Owner->IsCanceled(); }
	auto FRequestOwner::LaunchTask(std::string_view Name, TMoveOnlyFunction<void()>&& Work) -> void { Owner->LaunchTask(Name, std::move(Work)); }
	auto Private::FOwnerAccess::Invoke(IRequestOwner& Owner, TMoveOnlyFunction<void()>&& Callback) -> void
	{ FRequestBarrier Barrier(Owner, ERequestBarrierFlags::Priority); Callback(); }
	auto Private::FOwnerAccess::End(IRequestOwner& Owner, IRequest* Request, TMoveOnlyFunction<void()>&& Callback) -> TRefCountPtr<IRequest>
	{
		FRequestOwnerScope Scope(Request);
		return Owner.End(Request, Callback);
	}

	auto IRequestOwner::LaunchTask(std::string_view Name, TMoveOnlyFunction<void()>&& Work) -> void
	{
		struct FTaskRequest final : IRequest
		{
			Tasks::FTask Task;
			auto Cancel() -> void override { Wait(); }
			auto Wait() -> void override { requiref(WaitTask(Task).WaitStatus == ETaskWaitStatus::Completed, "Requests must be waited outside their callbacks."); }
			auto SetPriority(ERequestPriority) -> void override {}
		};
		if (!IsTaskSchedulerRunning()) { FRequestBarrier Barrier(*this, ERequestBarrierFlags::Priority); Work(); return; }
		Tasks::FTaskEvent Published("LaunchTaskRequest", false);
		auto Request = MakeRefCount<FTaskRequest>();
		const auto Prerequisites = Tasks::Prerequisites(Published);
		const std::string DebugName(Name);
		Tasks::FTaskExecutionOptions Options;
		Options.DebugName = DebugName.c_str(); Options.Prerequisites.assign(Prerequisites.begin(), Prerequisites.end());
		const auto Priority = GetPriority();
		Options.Priority = Priority == ERequestPriority::High ? ETaskPriority::High : Priority == ERequestPriority::Low ? ETaskPriority::Low : ETaskPriority::Normal;
		Request->Task = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, Options, [this, Request, Work = std::move(Work)]() mutable { End(Request, Work); });
		Begin(Request); Published.Trigger();
	}
}
