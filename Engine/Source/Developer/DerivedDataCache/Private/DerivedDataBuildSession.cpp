#include "DerivedDataBuildSession.h"
#include "DerivedDataBuildExecutionPrivate.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <thread>

namespace Durin::DerivedData
{
	namespace Private
	{
		struct FBuildRequestOwnerState
		{
			std::mutex Mutex;
			std::condition_variable Changed;
			bool Canceled = false;
			EBuildPriority Priority = EBuildPriority::Normal;
			uint64 Barriers = 0;
			std::unordered_map<std::thread::id, uint64> BarrierThreads;
			std::unordered_map<FBuildRequestState*, std::shared_ptr<FBuildRequestState>> Requests;
		};
		struct FSessionResources
		{
			FBuildRegistrySnapshot Registry;
			std::shared_ptr<const IBuildInputResolver> Resolver;
			std::shared_ptr<IBuildScheduler> Scheduler;
			FBuildServiceOptions Service;
		};
		struct FExecutionScope;
		thread_local FExecutionScope* CurrentScope = nullptr;
		struct FExecutionScope
		{
			FBuildSessionState* Owner;
			FBuildRequestState* Request = nullptr;
			FBuildRequestOwnerState* Group = nullptr;
			FExecutionScope* Previous = CurrentScope;
			explicit FExecutionScope(FBuildSessionState* Owner, FBuildRequestState* Request = nullptr, FBuildRequestOwnerState* Group = nullptr)
				: Owner(Owner), Request(Request), Group(Group) { CurrentScope = this; }
			~FExecutionScope() { CurrentScope = Previous; }
		};
		struct FBuildSessionState
		{
			std::mutex Mutex;
			std::condition_variable Changed;
			bool Closed = false, Released = false;
			uint64 NextId = 0, Active = 0, Submitting = 0;
			std::shared_ptr<FSessionResources> Resources;
			std::unordered_map<uint64, std::shared_ptr<FBuildRequestState>> Requests;
			auto Close() -> void;
			auto Drain() -> EBuildDrainResult;
			auto ReleaseIfDrained() -> void
			{
				std::shared_ptr<FSessionResources> Retired;
				{
					std::lock_guard Lock(Mutex);
					if (!Closed || Active != 0 || Submitting != 0 || !Resources) return;
					Retired = std::move(Resources);
				}
				FExecutionScope Scope(this);
				Retired.reset();
				{ std::lock_guard Lock(Mutex); Released = true; }
				Changed.notify_all();
			}
			auto Remove(uint64 Id) -> void
			{
				{ std::lock_guard Lock(Mutex); Requests.erase(Id); --Active; }
				ReleaseIfDrained();
				Changed.notify_all();
			}
		};
		struct FRequestData
		{
			std::variant<FBuildDefinition, FBuildAction> Request;
			FBuildInputs Inputs;
			FBuildRequestOptions Options;
			std::shared_ptr<FSessionResources> Resources;
		};
		enum class ERequestState { PendingDispatch, Queued, Running, Completing, Done };
		struct FBuildRequestState
		{
			std::mutex Mutex;
			std::atomic<bool> Canceled = false;
			std::condition_variable Changed;
			ERequestState State = ERequestState::PendingDispatch;
			bool Dispatching = true, CompletionReleased = false, OwnerReleased = false;
			std::shared_ptr<FBuildRequestOwnerState> Group;
			std::shared_ptr<IBuildScheduledWork> Scheduled;
			EBuildPriority Priority = EBuildPriority::Normal;
			uint64 Id = 0;
			std::weak_ptr<FBuildSessionState> Owner;
			std::shared_ptr<FRequestData> Data;
			FBuildCompletionCallback Callback;

			auto ReleaseOwner() -> void
			{
				std::shared_ptr<FBuildRequestOwnerState> Retired;
				{
					std::lock_guard Lock(Mutex);
					if (Dispatching || !CompletionReleased || OwnerReleased) return;
					OwnerReleased = true;
					Retired = Group;
				}
				if (Retired)
				{
					{ std::lock_guard Lock(Retired->Mutex); Retired->Requests.erase(this); }
					Retired->Changed.notify_all();
				}
				Changed.notify_all();
			}
			auto DispatchReturned() -> void
			{
				{ std::lock_guard Lock(Mutex); Dispatching = false; }
				ReleaseOwner();
				Changed.notify_all();
			}
			auto Wait() -> EBuildWaitResult
			{
				for (auto* Scope = CurrentScope; Scope; Scope = Scope->Previous)
					if (Scope->Request == this) return EBuildWaitResult::WouldBlock;
				std::shared_ptr<IBuildScheduledWork> Work;
				{
					std::unique_lock Lock(Mutex);
					Changed.wait(Lock, [&] { return !Dispatching; });
					if (CompletionReleased) return EBuildWaitResult::Completed;
					Work = Scheduled;
				}
				if (Work)
				{
					const auto Result = Work->Wait();
					if (Result != EBuildWaitResult::Completed) return Result;
				}
				std::unique_lock Lock(Mutex);
				Changed.wait(Lock, [&] { return CompletionReleased; });
				return EBuildWaitResult::Completed;
			}

			auto Finish(FBuildCompleteParams Completion) -> void
			{
				auto Session = Owner.lock();
				FBuildCompletionCallback Notify;
				std::shared_ptr<FRequestData> Retired;
				{
					std::lock_guard Lock(Mutex);
					if (State == ERequestState::Completing || State == ERequestState::Done) return;
					State = ERequestState::Completing;
					if (Canceled.load()) Completion = Private::FBuildCompletionAccess::Canceled(std::move(Completion));
					Notify = std::move(Callback);
					Retired = std::move(Data);
				}
				Retired.reset();
				FExecutionScope Scope(Session.get(), this, Group.get());
				try { if (Notify) Notify(std::move(Completion)); } catch (...) {}
				Notify = {};
				{ std::lock_guard Lock(Mutex); State = ERequestState::Done; }
				if (Session) Session->Remove(Id);
				{ std::lock_guard Lock(Mutex); CompletionReleased = true; }
				ReleaseOwner();
			}
			auto Cancel() -> bool
			{
				bool Complete = false;
				{
					std::lock_guard Lock(Mutex);
					if (State == ERequestState::Completing || State == ERequestState::Done) return false;
					Canceled.store(true);
					Complete = State == ERequestState::Queued;
				}
				if (Complete) Run();
				return true;
			}
			auto Run() -> void
			{
				auto Session = Owner.lock();
				std::shared_ptr<FRequestData> Work;
				{
					std::lock_guard Lock(Mutex);
					if (State != ERequestState::PendingDispatch && State != ERequestState::Queued) return;
					State = ERequestState::Running;
					Work = std::move(Data);
				}
				FExecutionScope Scope(Session.get(), this, Group.get());
				FBuildCompleteParams Completion = FBuildCompleteParams::Canceled(
					std::nullopt, EBuildStatus::None);
				try
				{
					FBuildCancellation ExternalCancellation = std::move(Work->Options.Cancellation);
					FBuildCancellation Token([&, ExternalCancellation = std::move(ExternalCancellation)] {
						return Canceled.load() || ExternalCancellation.IsCancelled();
					});
					Work->Options.Cancellation = std::move(Token);
					Completion = ExecuteBuild(Work->Request, Work->Resources->Registry,
						Work->Options.InputResolver ? Work->Options.InputResolver : Work->Resources->Resolver, Work->Inputs, Work->Options,
						Work->Resources->Service);
				}
				catch (...)
				{
					Completion = FBuildCompleteParams::Error(std::nullopt, EBuildStatus::None);
				}
				Work.reset();
				Finish(std::move(Completion));
			}
			auto AcceptDispatch() -> void
			{
				bool Complete = false;
				{
					std::lock_guard Lock(Mutex);
					if (State == ERequestState::PendingDispatch)
					{
						State = ERequestState::Queued;
						Complete = Canceled.load();
					}
				}
				if (Complete) Cancel();
			}
			auto RejectDispatch() -> bool
			{
				FBuildCompletionCallback RetiredCallback;
				std::shared_ptr<FRequestData> Retired;
				{
					std::lock_guard Lock(Mutex);
					if (State != ERequestState::PendingDispatch) return false;
					State = ERequestState::Done;
					Retired = std::move(Data);
					RetiredCallback = std::move(Callback);
				}
				Retired.reset(); RetiredCallback = {};
				if (auto Session = Owner.lock()) Session->Remove(Id);
				{ std::lock_guard Lock(Mutex); CompletionReleased = true; }
				ReleaseOwner();
				return true;
			}
		};
		auto FBuildSessionState::Close() -> void
		{
			decltype(Requests) Pending;
			{
				std::lock_guard Lock(Mutex);
				if (Closed) return;
				Closed = true; Pending = std::move(Requests);
			}
			for (const auto& [Id, Request] : Pending) Request->Cancel();
			ReleaseIfDrained();
		}
		auto FBuildSessionState::Drain() -> EBuildDrainResult
		{
			Close();
			for (auto* Scope = CurrentScope; Scope; Scope = Scope->Previous)
				if (Scope->Owner == this) return EBuildDrainResult::WouldBlock;
			std::unique_lock Lock(Mutex);
			Changed.wait(Lock, [&] { return Active == 0 && Submitting == 0 && Released; });
			return EBuildDrainResult::Drained;
		}
		struct FSubmissionScope
		{
			std::shared_ptr<FBuildSessionState> Owner;
			bool Counted = false;
			~FSubmissionScope()
			{
				if (!Counted) return;
				{ std::lock_guard Lock(Owner->Mutex); --Owner->Submitting; }
				Owner->ReleaseIfDrained(); Owner->Changed.notify_all();
			}
		};
		struct FDispatchTicket
		{
			std::shared_ptr<FBuildRequestState> Request;
			~FDispatchTicket() { Request->Cancel(); }
		};
		struct FDispatchReturnScope
		{
			std::shared_ptr<FBuildRequestState> Request;
			~FDispatchReturnScope() { if (Request) Request->DispatchReturned(); }
		};
		struct FBuildServiceState
		{
			std::mutex Mutex;
			bool Closed = false;
			FBuildRegistry Registry;
			FBuildServiceOptions Options;
			std::vector<std::weak_ptr<FBuildSessionState>> Sessions;
		};
	}

	class FBuildService final : public IBuild
	{
	public:
		explicit FBuildService(FBuildServiceOptions Options) : State(std::make_shared<Private::FBuildServiceState>())
		{ State->Options = std::move(Options); }
		~FBuildService() override { Close(); Drain(); }
		auto Register(std::shared_ptr<const IBuildFunction> Function)
			-> std::expected<void, FBuildAdmissionError> override
		{
			std::lock_guard Lock(State->Mutex);
			if (State->Closed) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::Closed, "Build service is closed."});
			auto Added = State->Registry.Register(std::move(Function));
			if (!Added)
			{
				return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::InvalidRequest, std::move(Added.error())});
			}
			return {};
		}
		auto CreateSession(std::shared_ptr<const IBuildInputResolver> Resolver,
			std::shared_ptr<IBuildScheduler> Scheduler) -> std::expected<std::shared_ptr<FBuildSession>, FBuildAdmissionError> override
		{
			std::lock_guard Lock(State->Mutex);
			if (State->Closed) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::Closed, "Build service is closed."});
			auto Snapshot = State->Registry.Freeze();
			if (!Snapshot) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::InternalFailure, std::move(Snapshot.error())});
			auto SessionState = std::make_shared<Private::FBuildSessionState>();
			SessionState->Resources = std::make_shared<Private::FSessionResources>(
				std::move(*Snapshot), std::move(Resolver), std::move(Scheduler), State->Options);
			std::erase_if(State->Sessions, [](const auto& Weak) { return Weak.expired(); });
			if (State->Sessions.size() >= 4096)
				return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::Capacity, "Build service session capacity is exhausted."});
			State->Sessions.push_back(SessionState);
			auto Session = std::shared_ptr<FBuildSession>(new FBuildSession(std::move(SessionState)));
			return Session;
		}
		auto Close() -> void override
		{
			std::vector<std::shared_ptr<Private::FBuildSessionState>> Sessions;
			{
				std::lock_guard Lock(State->Mutex);
				State->Closed = true;
				for (auto& Weak : State->Sessions) if (auto Session = Weak.lock()) Sessions.push_back(std::move(Session));
			}
			for (const auto& Session : Sessions) Session->Close();
		}
		auto Drain() -> EBuildDrainResult override
		{
			Close();
			std::vector<std::shared_ptr<Private::FBuildSessionState>> Sessions;
			{
				std::lock_guard Lock(State->Mutex);
				for (auto& Weak : State->Sessions) if (auto Session = Weak.lock()) Sessions.push_back(std::move(Session));
			}
			for (const auto& Session : Sessions)
				if (Session->Drain() == EBuildDrainResult::WouldBlock) return EBuildDrainResult::WouldBlock;
			return EBuildDrainResult::Drained;
		}
	private:
		std::shared_ptr<Private::FBuildServiceState> State;
	};

	auto CreateBuild(FBuildServiceOptions Options) -> std::shared_ptr<IBuild>
	{ return std::make_shared<FBuildService>(std::move(Options)); }
	auto GetBuild() -> IBuild&
	{
		static const std::shared_ptr<IBuild> Build = CreateBuild();
		return *Build;
	}

	FBuildSession::FBuildSession(std::shared_ptr<Private::FBuildSessionState> InState) : State(std::move(InState)) {}
	FBuildSession::~FBuildSession() { Close(); Drain(); }
	auto FBuildRequest::Cancel() const -> bool { return State && State->Cancel(); }
	auto FBuildRequest::Wait() const -> EBuildWaitResult
	{ return State ? State->Wait() : EBuildWaitResult::InvalidRequest; }

	FBuildRequestOwner::FBuildRequestOwner(EBuildPriority Priority) : State(std::make_shared<Private::FBuildRequestOwnerState>())
	{ State->Priority = Priority; }
	FBuildRequestOwner::~FBuildRequestOwner() { Cancel(); Wait(); }
	auto FBuildRequestOwner::Cancel() -> void
	{
		std::vector<std::shared_ptr<Private::FBuildRequestState>> Requests;
		{
			std::lock_guard Lock(State->Mutex);
			State->Canceled = true;
			for (const auto& [Id, Request] : State->Requests) Requests.push_back(Request);
		}
		for (const auto& Request : Requests) Request->Cancel();
	}
	auto FBuildRequestOwner::IsCanceled() const -> bool
	{ std::lock_guard Lock(State->Mutex); return State->Canceled; }
	auto FBuildRequestOwner::GetPriority() const -> EBuildPriority
	{ return State->Priority; }
	auto FBuildRequestOwner::Poll() const -> bool
	{ std::lock_guard Lock(State->Mutex); return State->Requests.empty() && State->Barriers == 0; }
	auto FBuildRequestOwner::Wait() const -> EBuildWaitResult
	{
		for (auto* Scope = Private::CurrentScope; Scope; Scope = Scope->Previous)
			if (Scope->Group == State.get()) return EBuildWaitResult::WouldBlock;
		for (;;)
		{
			std::shared_ptr<Private::FBuildRequestState> Request;
			{
				std::unique_lock Lock(State->Mutex);
				if (State->BarrierThreads.contains(std::this_thread::get_id())) return EBuildWaitResult::WouldBlock;
				if (State->Requests.empty() && State->Barriers == 0) return EBuildWaitResult::Completed;
				if (!State->Requests.empty()) Request = State->Requests.begin()->second;
				else { State->Changed.wait(Lock); continue; }
			}
			const auto Result = Request->Wait();
			if (Result != EBuildWaitResult::Completed) return Result;
		}
	}
	FBuildRequestBarrier::FBuildRequestBarrier(FBuildRequestOwner& Owner)
		: State(Owner.State), Thread(std::this_thread::get_id())
	{
		std::lock_guard Lock(State->Mutex);
		++State->BarrierThreads[Thread];
		++State->Barriers;
	}
	FBuildRequestBarrier::~FBuildRequestBarrier()
	{
		{
			std::lock_guard Lock(State->Mutex);
			if (--State->BarrierThreads.at(Thread) == 0)
				State->BarrierThreads.erase(Thread);
			--State->Barriers;
		}
		State->Changed.notify_all();
	}
	auto FBuildRequest::IsComplete() const -> bool
	{
		if (!State) return false;
		std::lock_guard Lock(State->Mutex);
		return State->State == Private::ERequestState::Done;
	}
	auto FBuildSession::Build(FBuildDefinition Definition, FBuildCompletionCallback Completion,
		FBuildInputs Inputs, FBuildRequestOptions Options) -> std::expected<FBuildRequest, FBuildAdmissionError>
	{ return BuildImpl(std::move(Definition), std::move(Completion), std::move(Inputs), std::move(Options)); }
	auto FBuildSession::Build(FBuildAction Action, FBuildCompletionCallback Completion,
		FBuildInputs Inputs, FBuildRequestOptions Options) -> std::expected<FBuildRequest, FBuildAdmissionError>
	{ return BuildImpl(std::move(Action), std::move(Completion), std::move(Inputs), std::move(Options)); }
	auto FBuildSession::Build(FBuildDefinition Definition, FBuildRequestOwner& Owner,
		FBuildCompletionCallback Completion, FBuildInputs Inputs, FBuildRequestOptions Options)
		-> std::expected<FBuildRequest, FBuildAdmissionError>
	{ return BuildImpl(std::move(Definition), std::move(Completion), std::move(Inputs), std::move(Options), Owner.State); }
	auto FBuildSession::Build(FBuildAction Action, FBuildRequestOwner& Owner,
		FBuildCompletionCallback Completion, FBuildInputs Inputs, FBuildRequestOptions Options)
		-> std::expected<FBuildRequest, FBuildAdmissionError>
	{ return BuildImpl(std::move(Action), std::move(Completion), std::move(Inputs), std::move(Options), Owner.State); }
	auto FBuildSession::BuildImpl(std::variant<FBuildDefinition, FBuildAction> BuildRequest,
		FBuildCompletionCallback Completion, FBuildInputs Inputs, FBuildRequestOptions Options,
		std::shared_ptr<Private::FBuildRequestOwnerState> RequestOwner)
		-> std::expected<FBuildRequest, FBuildAdmissionError>
	{
		auto Owner = State;
		Private::FSubmissionScope Submission{Owner};
		Private::FDispatchReturnScope DispatchReturn;
		Private::FExecutionScope Execution(Owner.get());
		std::shared_ptr<Private::FBuildRequestState> Request;
		try
		{
			std::shared_ptr<IBuildScheduler> Scheduler;
			{
				std::lock_guard Lock(Owner->Mutex);
				if (Owner->Closed) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::Closed, "Build session is closed."});
				if (!Completion) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::InvalidRequest, "Build completion callback is missing."});
				if (Inputs.IsValid() && Options.InputResolver)
					return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::InvalidRequest, "Build inputs and a request resolver are mutually exclusive."});
				if (Owner->Active >= 4096) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::Capacity, "Build session is full."});
				const std::string_view Name = std::holds_alternative<FBuildDefinition>(BuildRequest)
					? std::get<FBuildDefinition>(BuildRequest).GetFunctionName() : std::get<FBuildAction>(BuildRequest).GetFunction().Name;
				const auto Entry = Owner->Resources->Registry.Find(Name);
				if (!Entry)
					return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::MissingFunction, "Build function is not registered."});
				if (const auto* Action = std::get_if<FBuildAction>(&BuildRequest);
					Action && Action->GetFunction() != Entry->Descriptor)
					return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::InvalidRequest,
						"Build action does not match the registered function."});
				Request = std::make_shared<Private::FBuildRequestState>();
				Request->Id = Owner->NextId++;
				Request->Owner = Owner;
				Request->Data = std::make_shared<Private::FRequestData>(
					std::move(BuildRequest), std::move(Inputs), std::move(Options), Owner->Resources);
				Request->Callback = std::move(Completion);
				Scheduler = Owner->Resources->Scheduler;
				Owner->Requests.emplace(Request->Id, Request); ++Owner->Active;
				++Owner->Submitting; Submission.Counted = true;
				DispatchReturn.Request = Request;
				if (RequestOwner)
				{
					std::lock_guard GroupLock(RequestOwner->Mutex);
					RequestOwner->Requests.emplace(Request.get(), Request);
					Request->Group = RequestOwner;
					Request->Canceled = RequestOwner->Canceled;
					Request->Priority = RequestOwner->Priority;
					Execution.Group = RequestOwner.get();
				}
				Execution.Request = Request.get();
			}
			FBuildRequest Handle; Handle.State = Request;
			if (!Scheduler || Request->Canceled.load()) { Request->Run(); return Handle; }
			auto Ticket = std::make_shared<Private::FDispatchTicket>(); Ticket->Request = Request;
			const auto& Data = *Request->Data;
			const std::string FunctionName(std::holds_alternative<FBuildDefinition>(Data.Request)
				? std::get<FBuildDefinition>(Data.Request).GetFunctionName() : std::get<FBuildAction>(Data.Request).GetFunction().Name);
			auto Accepted = Scheduler->Schedule({FunctionName, Request->Priority, Data.Options.Policy.MaximumWorkingSetBytes},
				[Ticket] { Ticket->Request->Run(); });
			if (Accepted) { std::lock_guard Lock(Request->Mutex); Request->Scheduled = std::move(*Accepted); }
			Ticket.reset();
			if (!Accepted && Request->RejectDispatch())
			{
				auto Error = std::move(Accepted.error()); Error.BoundDescription();
				return std::unexpected(std::move(Error));
			}
			Request->AcceptDispatch();
			return Handle;
		}
		catch (...)
		{
			if (Submission.Counted && !Request->RejectDispatch()) { FBuildRequest Handle; Handle.State = Request; return Handle; }
			return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::InternalFailure, "Build dispatch or admission failed."});
		}
	}
	auto FBuildSession::Close() -> void
	{ State->Close(); }
	auto FBuildSession::Drain() -> EBuildDrainResult
	{ return State->Drain(); }
}
