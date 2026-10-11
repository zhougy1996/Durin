#include "DerivedDataBuildSession.h"
#include "DerivedDataBuildExecutionPrivate.h"
#include "DerivedDataCache/DerivedDataCacheAsync.h"
#include "Threading/TaskComposition.h"
#include "Logging/LogMacros.h"

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
			std::shared_ptr<FCacheRequestOwner> CacheOwner;
			std::optional<FBuildCompleteParams> LocalCompletion;
		};
		enum class ERequestState { PendingDispatch, Queued, Running, Completing, Done };
		struct FBuildRequestState : std::enable_shared_from_this<FBuildRequestState>
		{
			std::mutex Mutex;
			std::atomic<bool> Canceled = false;
			std::condition_variable Changed;
			ERequestState State = ERequestState::PendingDispatch;
			bool Dispatching = true, CompletionReleased = false, OwnerReleased = false;
			std::shared_ptr<FBuildRequestOwnerState> Group;
			std::shared_ptr<IBuildScheduledWork> Scheduled;
			uint64 ScheduleRevision = 0;
			bool StageDispatching = false;
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
				for (;;)
				{
					std::shared_ptr<IBuildScheduledWork> Work; uint64 Revision;
					{
						std::unique_lock Lock(Mutex); Changed.wait(Lock, [&] { return !Dispatching && (!StageDispatching || CompletionReleased); });
						if (CompletionReleased) return EBuildWaitResult::Completed;
						Work = Scheduled; Revision = ScheduleRevision;
					}
					if (Work && Work->Wait() != EBuildWaitResult::Completed) return EBuildWaitResult::WouldBlock;
					std::unique_lock Lock(Mutex);
					Changed.wait(Lock, [&] { return CompletionReleased || ScheduleRevision != Revision; });
				}
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
				FExecutionScope Scope(Session.get(), this, Group.get());
				Retired.reset();
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
				std::shared_ptr<FCacheRequestOwner> CacheOwner;
				{
					std::lock_guard Lock(Mutex);
					if (State == ERequestState::Completing || State == ERequestState::Done) return false;
					Canceled.store(true);
					Complete = State == ERequestState::Queued;
					if (Data) CacheOwner = Data->CacheOwner;
				}
				if (CacheOwner) CacheOwner->Cancel();
				if (Complete) Run();
				return true;
			}
			auto BeginScheduledStage() -> uint64
			{
				uint64 Revision;
				{ std::lock_guard Lock(Mutex); Scheduled.reset(); StageDispatching = true; Revision = ++ScheduleRevision; }
				Changed.notify_all(); return Revision;
			}
			auto ResumeLocal() -> void;
			auto CacheFailure(std::string_view Operation, const FCacheError& Error) noexcept -> void
			{
				try
				{
					auto Work = Data; const auto& Action = std::get<FBuildAction>(Work->Request);
					DURIN_WARN_CATEGORY("DerivedData", "{} cache {} [{}]: {}", Action.GetFunction().Name, Operation, Action.GetKey().ToString(), Error.Diagnostic);
				}
				catch (...) {}
			}
			auto InfrastructureFailure(bool IsCanceled = false) -> void
			{
				std::optional<FCacheKey> Key;
				{ std::lock_guard Lock(Mutex); if (Data) if (const auto* Action = std::get_if<FBuildAction>(&Data->Request)) Key = Action->GetKey(); }
				Finish(IsCanceled ? FBuildCompleteParams::Canceled(Key, EBuildStatus::None) : FBuildCompleteParams::Error(Key, EBuildStatus::None));
			}
			auto StartAsync(std::shared_ptr<FRequestData> Work) -> void
			{
				auto Action = PrepareBuildAction(Work->Request, Work->Resources->Registry,
					Work->Options.InputResolver ? Work->Options.InputResolver : Work->Resources->Resolver, Work->Inputs, Work->Options.Cancellation);
				if (!Action) { Finish(std::move(Action.error())); return; }
				Work->Request = std::move(*Action); Work->CacheOwner = std::make_shared<FCacheRequestOwner>(Priority);
				{ std::lock_guard Lock(Mutex); Data = std::move(Work); }
				const auto Policy = Data->Options.Policy;
				if (!Policy.QueryCache || Policy.ForceBuild) { ContinueLocal(); return; }
				const std::array Requests{FCacheGetRequest{.Key = std::get<FBuildAction>(Data->Request).GetKey(), .OutputLimits = Policy.OutputLimits,
					.MaximumEncodedBytes = Policy.MaximumEncodedBytes, .MaximumWorkingSetBytes = Policy.MaximumWorkingSetBytes,
					.ShouldCancel = [Cancel = Data->Options.Cancellation] { return Cancel.IsCancelled(); }}};
				auto Cache = Data->Resources->Service.AsyncCache; auto CacheOwner = Data->CacheOwner;
				auto Self = shared_from_this(); const auto Revision = BeginScheduledStage();
				auto Submitted = Cache->Get(Requests, *CacheOwner,
					[Self](uint32, FCacheGetResult Result, FCacheQueryStats) { Self->ReceiveCache(std::move(Result)); });
				if (!Submitted) ReceiveCache(std::unexpected(std::move(Submitted.error())));
				else SetScheduledIfCurrent(std::move(*Submitted), Revision);
			}
			auto SetScheduledIfCurrent(std::shared_ptr<IBuildScheduledWork> Work, uint64 Revision) -> void
			{
				{ std::lock_guard Lock(Mutex); if (ScheduleRevision != Revision || State == ERequestState::Done) return; Scheduled = std::move(Work); StageDispatching = false; }
				Changed.notify_all();
			}
			auto ReceiveCache(FCacheGetResult Result) -> void
			try
			{
				auto Session = Owner.lock(); FExecutionScope Scope(Session.get(), this, Group.get());
				auto Work = Data;
				const auto Key = std::get<FBuildAction>(Work->Request).GetKey();
				if (Work->Options.Cancellation.IsCancelled()) { Work.reset(); Finish(FBuildCompleteParams::Canceled(Key, EBuildStatus::None)); return; }
				if (Result && *Result)
				{
					auto Output = (**Result).ToOutput(Key, Work->Options.Policy.OutputLimits);
					if (Output) { Work.reset(); Finish(FBuildCompletionAccess::Ok(std::move(*Output), Key, EBuildStatus::CacheQueryHit)); return; }
					Result = std::unexpected(std::move(Output.error()));
				}
				if (!Result) CacheFailure("read", Result.error());
				Work.reset(); ContinueLocal();
			}
			catch (...) { InfrastructureFailure(); }
			auto ContinueLocal() -> void;
			auto StoreLocal(std::shared_ptr<FRequestData> Work, FBuildCompleteParams Completion) -> void
			try
			{
				const auto* Output = Completion.GetOutput();
				if (!Output || !Work->Options.Policy.StoreOnBuild || Output->HasLogs()) { Work.reset(); Finish(std::move(Completion)); return; }
				const auto Policy = Work->Options.Policy;
				Work->LocalCompletion = std::move(Completion);
				auto Record = FCacheRecord::FromOutput(*Work->LocalCompletion->GetCacheKey(), *Work->LocalCompletion->GetOutput(), Policy.PersistenceLimits);
				if (!Record) { Work.reset(); ReceiveStore(std::unexpected(std::move(Record.error()))); return; }
				const std::array Requests{FCachePutRequest{std::move(*Record), Policy.MaximumEncodedBytes, Policy.OutputLimits.MaximumTotalBytes, Policy.MaximumWorkingSetBytes,
					[Cancel = Work->Options.Cancellation] { return Cancel.IsCancelled(); }}};
				auto Cache = Work->Resources->Service.AsyncCache; auto CacheOwner = Work->CacheOwner; Work.reset();
				auto Self = shared_from_this(); const auto Revision = BeginScheduledStage();
				auto Saved = Cache->Put(Requests, *CacheOwner,
					[Self](uint32, FCachePutResult Result) { Self->ReceiveStore(std::move(Result)); });
				if (!Saved) ReceiveStore(std::unexpected(std::move(Saved.error())));
				else SetScheduledIfCurrent(std::move(*Saved), Revision);
			}
			catch (...) { Work.reset(); ReceiveStore(std::unexpected(FCacheError{ECacheError::StorageFailure, "Cache persistence threw."})); }
			auto ReceiveStore(FCachePutResult Result) -> void
			{
				auto Session = Owner.lock(); FExecutionScope Scope(Session.get(), this, Group.get());
				if (!Data || !Data->LocalCompletion) return;
				if (!Result) CacheFailure("write", Result.error());
				auto Completion = std::move(*Data->LocalCompletion);
				Data->LocalCompletion.reset();
				try { if (Data->Options.Cancellation.IsCancelled()) Completion = FBuildCompletionAccess::Canceled(std::move(Completion)); }
				catch (...) { Completion = FBuildCompleteParams::Error(*Completion.GetCacheKey(), Completion.GetBuildStatus()); }
				Finish(std::move(Completion));
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
					FBuildCancellation Token([Weak = weak_from_this(), ExternalCancellation = std::move(ExternalCancellation)] {
						auto Self = Weak.lock(); return !Self || Self->Canceled.load() || ExternalCancellation.IsCancelled();
					});
					Work->Options.Cancellation = std::move(Token);
					if (Work->Resources->Scheduler && Work->Resources->Service.AsyncCache
						&& (!Work->Resources->Scheduler->SupportsContinuations() || IsTaskSchedulerRunning()))
					{ StartAsync(std::move(Work)); return; }
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
		struct FContinuationTicket
		{
			std::shared_ptr<FBuildRequestState> Request;
			bool Started = false;
			~FContinuationTicket() { if (!Started) Request->InfrastructureFailure(true); }
			auto Run() -> void { Started = true; Request->ResumeLocal(); }
		};
		auto FBuildRequestState::ContinueLocal() -> void
		try
		{
			auto Work = Data;
			if (Work->Options.Cancellation.IsCancelled()) { const auto Key = std::get<FBuildAction>(Work->Request).GetKey(); Work.reset(); Finish(FBuildCompleteParams::Canceled(Key, EBuildStatus::None)); return; }
			auto Scheduler = Work->Resources->Scheduler;
			const auto Function = std::get<FBuildAction>(Work->Request).GetFunction().Name;
			const auto Budget = Work->Options.Policy.MaximumWorkingSetBytes; Work.reset();
			auto Ticket = std::make_shared<FContinuationTicket>(); Ticket->Request = shared_from_this();
			const auto Revision = BeginScheduledStage();
			auto Accepted = Scheduler->Schedule({Function, Priority, Budget}, [Ticket] { Ticket->Run(); });
			if (!Accepted) InfrastructureFailure();
			else SetScheduledIfCurrent(std::move(*Accepted), Revision);
		}
		catch (...) { InfrastructureFailure(); }
		auto FBuildRequestState::ResumeLocal() -> void
		{
			auto Session = Owner.lock(); FExecutionScope Scope(Session.get(), this, Group.get());
			auto Work = Data;
			try
			{
				auto Options = Work->Options; Options.Policy.QueryCache = false; Options.Policy.StoreOnBuild = false;
				auto Completion = ExecuteBuild(Work->Request, Work->Resources->Registry,
					Options.InputResolver ? Options.InputResolver : Work->Resources->Resolver, Work->Inputs, Options, Work->Resources->Service);
				Options = {}; Work->Inputs = {}; Work->Options.InputResolver.reset();
				StoreLocal(std::move(Work), std::move(Completion));
			}
			catch (...) { Work.reset(); InfrastructureFailure(); }
		}
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
			bool Started = false;
			~FDispatchTicket() { if (!Started) Request->Cancel(); }
			auto Run() -> void { Started = true; Request->Run(); }
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
			std::shared_ptr<IAsyncCache> DefaultAsyncCache;
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
			auto SessionOptions = State->Options;
			if (Scheduler && Scheduler->SupportsContinuations() && !SessionOptions.AsyncCache)
			{
				if (!State->DefaultAsyncCache) State->DefaultAsyncCache = CreateAsyncCache(State->Options.Cache);
				SessionOptions.AsyncCache = State->DefaultAsyncCache;
			}
			auto SessionState = std::make_shared<Private::FBuildSessionState>();
			SessionState->Resources = std::make_shared<Private::FSessionResources>(
				std::move(*Snapshot), std::move(Resolver), std::move(Scheduler), std::move(SessionOptions));
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
			const auto Revision = Request->BeginScheduledStage();
			auto Accepted = Scheduler->Schedule({FunctionName, Request->Priority, Data.Options.Policy.MaximumWorkingSetBytes},
				[Ticket] { Ticket->Run(); });
			if (Accepted) Request->SetScheduledIfCurrent(std::move(*Accepted), Revision);
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
