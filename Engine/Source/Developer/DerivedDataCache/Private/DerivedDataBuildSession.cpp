#include "BuildCacheRecordPrivate.h"
#include "DerivedDataBuildSession.h"
#include "DerivedDataBuildExecutionPrivate.h"
#include "CacheDispatchPrivate.h"
#include "BuildServiceBackendPrivate.h"
#include "Logging/LogMacros.h"
#include "RequestOwnerPrivate.h"
#include "CacheDiagnosticsPrivate.h"
#include "DerivedDataCache/DerivedDataCache.h"
#include "Threading/Task.h"

namespace Durin::DerivedData
{
	namespace Private
	{
		struct FSessionResources
		{
			FBuildRegistrySnapshot Registry;
			std::shared_ptr<const IBuildInputResolver> Resolver;
			std::shared_ptr<IBuildScheduler> Scheduler;
			std::shared_ptr<ICache> Cache;
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
				FRequestOwnerScope Scope(nullptr, nullptr, this);
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
			FBuildCancellation Cancellation;
			std::shared_ptr<FSessionResources> Resources;
			std::optional<FBuildCompleteParams> LocalCompletion;
		};
		class FCacheOwnerWait final : public IBuildScheduledWork
		{
		public:
			std::shared_ptr<FRequestOwner> Owner;
			explicit FCacheOwnerWait(std::shared_ptr<FRequestOwner> Owner) : Owner(std::move(Owner)) {}
			auto Wait() const -> ERequestWaitResult override { return Owner->Wait(); }
		};
		enum class ERequestState { PendingDispatch, Queued, Running, Completing, Done };
		struct FBuildRequestState : FRequestCompletion, std::enable_shared_from_this<FBuildRequestState>
		{
			Tasks::FCancellationToken Cancellation;
			std::shared_ptr<FRequestOwner> CacheOwner = std::make_shared<FRequestOwner>();
			std::shared_ptr<IBuildOperation> Operation;
			ERequestState State = ERequestState::PendingDispatch;
			uint64 Id = 0;
			std::weak_ptr<FBuildSessionState> Owner;
			std::shared_ptr<FRequestData> Data;
			FBuildCompletionCallback Callback;

			auto Finish(FBuildCompleteParams Completion) -> void
			{
				const bool bCancelled = Cancellation.IsCanceled();
				auto Session = Owner.lock();
				FBuildCompletionCallback Notify;
				std::shared_ptr<FRequestData> Retired;
				{
					std::lock_guard Lock(Mutex);
					if (State == ERequestState::Completing || State == ERequestState::Done) return;
					State = ERequestState::Completing;
					if (bCancelled || Cancellation.IsCanceled()) Completion = Private::FBuildCompletionAccess::Canceled(std::move(Completion));
					if (Completion.GetStatus() == EStatus::Canceled) Cancellation.Cancel();
					Notify = std::exchange(Callback, {});
					Retired = std::move(Data);
				}
				Complete([&] {
					Retired.reset();
					try { if (Notify) Notify(std::move(Completion)); } catch (...) {}
					Notify = {};
					{ std::lock_guard Lock(Mutex); State = ERequestState::Done; }
					if (Session) Session->Remove(Id);
				}, Session.get());
			}
			auto RequestCancel() -> bool override
			{
				bool Complete = false;
				std::shared_ptr<IBuildOperation> Work;
				{
					std::lock_guard Lock(Mutex);
					if (State == ERequestState::Completing || State == ERequestState::Done) return false;
					Cancellation.Cancel();
					Complete = State == ERequestState::Queued;
					Work = Operation;
				}
				if (Complete) Run();
				CacheOwner->RequestCancel();
				if (Work) Work->RequestCancel();
				return true;
			}
			auto BeginScheduledStage() -> uint64
			{
				uint64 Revision;
				{ std::lock_guard Lock(Mutex); Operation.reset(); Scheduled.reset(); StageDispatching = true; Revision = ++ScheduleRevision; }
				Changed.notify_all(); return Revision;
			}
			auto ResumeLocal() -> void;
			auto BeginCacheStage() -> void
			{
				CacheOwner->SetPriority(Group->GetPriority());
				std::lock_guard Lock(Mutex);
				Operation.reset(); Scheduled = std::make_shared<FCacheOwnerWait>(CacheOwner);
				StageDispatching = false; ++ScheduleRevision; Changed.notify_all();
			}
			auto InfrastructureFailure(bool IsCanceled = false) -> void
			{
				std::optional<FCacheKey> Key;
				{ std::lock_guard Lock(Mutex); if (Data) if (const auto* Action = std::get_if<FBuildAction>(&Data->Request)) Key = Action->GetKey(); }
				Finish(IsCanceled ? FBuildCompleteParams::Canceled(Key, EBuildExecutionFlags::None) : FBuildCompleteParams::Error(Key, EBuildExecutionFlags::None));
			}
			auto StartBuild(std::shared_ptr<FRequestData> Work) -> void
			{
				auto Action = PrepareBuildAction(Work->Request, Work->Resources->Registry,
					Work->Options.InputResolver ? Work->Options.InputResolver : Work->Resources->Resolver, Work->Inputs, Work->Cancellation);
				if (!Action) { Finish(std::move(Action.error())); return; }
				Work->Request = std::move(*Action);
				{ std::lock_guard Lock(Mutex); Data = std::move(Work); }
				const auto Policy = Data->Options.Policy;
				if (!Policy.Has(EBuildPolicy::CacheQuery)) { ResumeLocal(); return; }
				const auto& PreparedAction = std::get<FBuildAction>(Data->Request);
				const std::array Requests{FCacheGetRequest{.Key = PreparedAction.GetKey(), .Name = PreparedAction.GetFunction().Name}};
				auto Cache = Data->Resources->Cache;
				FRequestBarrier Submission(*CacheOwner);
				BeginCacheStage();
				auto Self = shared_from_this();
				Cache->Get(Requests, *CacheOwner, [Self](FCacheGetResponse Response) { Self->ReceiveCache(std::move(Response)); });
			}
			auto SetScheduledIfCurrent(std::shared_ptr<IBuildOperation> Work, uint64 Revision) -> void
			{
				{ std::lock_guard Lock(Mutex); if (ScheduleRevision != Revision || State == ERequestState::Done) return; Operation = Work; Scheduled = Work; StageDispatching = false; }
				if (Cancellation.IsCanceled() && Work) Work->RequestCancel();
				Changed.notify_all();
			}
			auto ReceiveCache(FCacheGetResponse Response) -> void
			try
			{
				auto Session = Owner.lock(); FRequestOwnerScope Scope(Group.get(), this, Session.get());
				auto Work = Data;
				const auto Key = std::get<FBuildAction>(Work->Request).GetKey();
				if (Work->Cancellation.IsCancelled() || Response.Status == EStatus::Canceled) { Work.reset(); Finish(FBuildCompleteParams::Canceled(Key, EBuildExecutionFlags::None)); return; }
				if (Response.Status == EStatus::Ok)
				{
					const auto& Function = std::get<FBuildAction>(Work->Request).GetFunction();
					auto Output = Private::ReadBuildOutput(Key, Response.Record, Work->Options.Limits.Output);
					const bool SchemaMismatch = Output && (Output->GetSchema() != Function.OutputType || Output->GetSchemaVersion() != Function.OutputSchema);
					if (Output && !SchemaMismatch && !Output->HasError()) { Work.reset(); Finish(FBuildCompletionAccess::Ok(std::move(*Output), Key, EBuildExecutionFlags::CacheQueryHit)); return; }
					Private::ObserveCache(Key, SchemaMismatch ? Private::ECacheObservation::SchemaRejected : Private::ECacheObservation::RecordRejected);
				}
				if (Work->Options.Policy.Has(EBuildPolicy::BuildLocal)) Private::ObserveCache(Key, Private::ECacheObservation::Fallback);
				Work.reset(); ContinueLocal();
			}
			catch (...) { InfrastructureFailure(); }
			auto ContinueLocal() -> void;
			auto StoreLocal(std::shared_ptr<FRequestData> Work, FBuildCompleteParams Completion) -> void
			try
			{
				const auto* Output = Completion.GetOutput();
				if (Completion.GetStatus() != EStatus::Ok || !Output || !Work->Options.Policy.Has(EBuildPolicy::CacheStoreOnBuild)
					|| Output->HasLogs()) { Work.reset(); Finish(std::move(Completion)); return; }
				Work->LocalCompletion = std::move(Completion);
				auto Record = Durin::DerivedData::Private::MakeBuildCacheRecord(*Work->LocalCompletion->GetCacheKey(), *Work->LocalCompletion->GetOutput(), Work->Options.Limits.Output);
				if (!Record) { Work.reset(); ReceiveStore({.Status = EStatus::Error}); return; }
				const std::array Requests{FCachePutRequest{.Record = std::move(*Record), .Name = std::get<FBuildAction>(Work->Request).GetFunction().Name}};
				auto Cache = Work->Resources->Cache; Work.reset();
				FRequestBarrier Submission(*CacheOwner);
				BeginCacheStage();
				auto Self = shared_from_this();
				Cache->Put(Requests, *CacheOwner, [Self](FCachePutResponse Response) { Self->ReceiveStore(std::move(Response)); });
			}
			catch (...) { Work.reset(); ReceiveStore({.Status = EStatus::Error}); }
			auto ReceiveStore(FCachePutResponse Response) -> void
			{
				auto Session = Owner.lock(); FRequestOwnerScope Scope(Group.get(), this, Session.get());
				if (!Data || !Data->LocalCompletion) return;
				auto Completion = std::move(*Data->LocalCompletion);
				Data->LocalCompletion.reset();
				try { if (Data->Cancellation.IsCancelled() || Response.Status == EStatus::Canceled) Completion = FBuildCompletionAccess::Canceled(std::move(Completion)); }
				catch (...) { Completion = FBuildCompleteParams::Error(*Completion.GetCacheKey(), Completion.GetExecutionFlags()); }
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
				FRequestOwnerScope Scope(Group.get(), this, Session.get());
				FBuildCompleteParams Completion = FBuildCompleteParams::Canceled(
					std::nullopt, EBuildExecutionFlags::None);
				try
				{
					Work->Cancellation = FBuildExecutionAccess::ObserveCancellation([Weak = weak_from_this()] {
						auto Self = Weak.lock();
						if (!Self) return true;
						if (Self->Group->IsCanceled()) Self->Cancellation.Cancel();
						return Self->Cancellation.IsCanceled();
					});
					StartBuild(std::move(Work));
					return;
				}
				catch (...)
				{
					Completion = FBuildCompleteParams::Error(std::nullopt, EBuildExecutionFlags::None);
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
						Complete = Cancellation.IsCanceled();
					}
				}
				if (Complete) RequestCancel();
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
				Complete();
				return true;
			}
		};
		auto FBuildRequestState::ContinueLocal() -> void
		try
		{
			auto Work = Data;
			if (Work->Cancellation.IsCancelled()) { const auto Key = std::get<FBuildAction>(Work->Request).GetKey(); Work.reset(); Finish(FBuildCompleteParams::Canceled(Key, EBuildExecutionFlags::None)); return; }
			auto Scheduler = Work->Resources->Scheduler;
			const auto Function = std::get<FBuildAction>(Work->Request).GetFunction().Name;
			Work.reset();
			auto Self = shared_from_this();
			const auto Revision = BeginScheduledStage();
			auto Accepted = Group->LaunchTask(this, *Scheduler, Function, [Self] { Self->ResumeLocal(); }, [Self] { Self->InfrastructureFailure(true); });
			if (!Accepted) InfrastructureFailure();
			else SetScheduledIfCurrent(std::move(*Accepted), Revision);
		}
		catch (...) { InfrastructureFailure(); }
		auto FBuildRequestState::ResumeLocal() -> void
		{
			auto Session = Owner.lock(); FRequestOwnerScope Scope(Group.get(), this, Session.get());
			auto Work = Data;
			try
			{
				auto Completion = ExecuteBuildLocal(std::get<FBuildAction>(Work->Request), Work->Resources->Registry,
					Work->Options.InputResolver ? Work->Options.InputResolver : Work->Resources->Resolver, Work->Inputs, Work->Options, Work->Cancellation);
				Work->Inputs = {}; Work->Options.InputResolver.reset();
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
			for (const auto& [Id, Request] : Pending) Request->RequestCancel();
			ReleaseIfDrained();
		}
		auto FBuildSessionState::Drain() -> EBuildDrainResult
		{
			Close();
			if (FRequestOwnerScope::Blocks(nullptr, this)) return EBuildDrainResult::WouldBlock;
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
			std::shared_ptr<ICache> InlineCache, TaskCache;
			std::shared_ptr<ICacheBackend> Backend;
			std::vector<std::weak_ptr<FBuildSessionState>> Sessions;
		};
	}

	class FBuildService final : public IBuild
	{
	public:
		explicit FBuildService(FBuildServiceOptions Options, std::shared_ptr<ICacheBackend> Backend = {}) : State(std::make_shared<Private::FBuildServiceState>())
		{ State->Options = std::move(Options); State->Backend = std::move(Backend); }
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
			std::shared_ptr<IBuildScheduler> Scheduler, EBuildCacheExecution CacheExecution) -> std::expected<std::shared_ptr<FBuildSession>, FBuildAdmissionError> override
		{
			std::lock_guard Lock(State->Mutex);
			if (State->Closed) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::Closed, "Build service is closed."});
			if (CacheExecution > EBuildCacheExecution::Task)
				return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::InvalidRequest, "Invalid cache execution mode."});
			auto Snapshot = State->Registry.Freeze();
			if (!Snapshot) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::InternalFailure, std::move(Snapshot.error())});
			auto SessionCache = State->Options.Cache;
			if (!Scheduler) Scheduler = CreateInlineBuildScheduler();
			if (!SessionCache) {
				auto& Cache = CacheExecution == EBuildCacheExecution::Inline ? State->InlineCache : State->TaskCache;
				if (!Cache) Cache = Private::CreateCacheForBackend(State->Backend,
					CacheExecution == EBuildCacheExecution::Task ? CreateTaskCacheScheduler() : CreateInlineBuildScheduler(),
					State->Options.CacheDispatchLimits, State->Options.CacheLimits);
				SessionCache = Cache;
			}
			auto SessionState = std::make_shared<Private::FBuildSessionState>();
			SessionState->Resources = std::make_shared<Private::FSessionResources>(
				std::move(*Snapshot), std::move(Resolver), std::move(Scheduler), std::move(SessionCache)
			);
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
	auto Private::CreateBuildForBackend(FBuildBackendOptions Options) -> std::shared_ptr<IBuild>
	{
		return std::make_shared<FBuildService>(FBuildServiceOptions{.CacheDispatchLimits = Options.CacheDispatchLimits, .CacheLimits = Options.CacheLimits}, std::move(Options.Cache));
	}
	auto GetBuild() -> IBuild&
	{
		static const std::shared_ptr<IBuild> Build = CreateBuild();
		return *Build;
	}

	FBuildSession::FBuildSession(std::shared_ptr<Private::FBuildSessionState> InState) : State(std::move(InState)) {}
	FBuildSession::~FBuildSession() { Close(); Drain(); }
	auto FBuildSession::Build(FBuildDefinition Definition, IRequestOwner& Owner, FBuildCompletionCallback Completion, FBuildInputs Inputs, FBuildRequestOptions Options) -> void
	{
		require(static_cast<bool>(Completion));
		FRequestBarrier Submission(Owner);
		auto Fallback = Completion;
		auto Result = BuildImpl(std::move(Definition), std::move(Completion), std::move(Inputs), std::move(Options), Owner);
		if (!Result)
		{
			try
			{
				Fallback(FBuildCompleteParams::Error({}, EBuildExecutionFlags::None, std::move(Result.error().Description)));
			}
			catch (...)
			{
			}
		}
	}
	auto FBuildSession::Build(FBuildAction Action, IRequestOwner& Owner, FBuildCompletionCallback Completion, FBuildInputs Inputs, FBuildRequestOptions Options) -> void
	{
		require(static_cast<bool>(Completion));
		FRequestBarrier Submission(Owner);
		auto Fallback = Completion;
		auto Result = BuildImpl(std::move(Action), std::move(Completion), std::move(Inputs), std::move(Options), Owner);
		if (!Result)
		{
			try
			{
				Fallback(FBuildCompleteParams::Error({}, EBuildExecutionFlags::None, std::move(Result.error().Description)));
			}
			catch (...)
			{
			}
		}
	}
	auto FBuildSession::BuildImpl(std::variant<FBuildDefinition, FBuildAction> BuildRequest, FBuildCompletionCallback Completion, FBuildInputs Inputs, FBuildRequestOptions Options, IRequestOwner& RequestOwner)
		-> std::expected<void, FBuildAdmissionError>
	{
		auto Owner = State;
		Private::FSubmissionScope Submission{Owner};
		Private::FDispatchReturnScope DispatchReturn;
		Private::FRequestOwnerScope Execution(nullptr, nullptr, Owner.get());
		std::shared_ptr<Private::FBuildRequestState> Request;
		try
		{
			std::shared_ptr<IBuildScheduler> Scheduler;
			{
				std::lock_guard Lock(Owner->Mutex);
				if (Owner->Closed) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::Closed, "Build session is closed."});
				if ((Options.Policy.GetCombinedPolicy() & ~EBuildPolicy::Default) != EBuildPolicy::None)
					return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::InvalidRequest, "Unsupported build policy flags."});
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
					std::move(BuildRequest), std::move(Inputs), std::move(Options), FBuildCancellation{}, Owner->Resources);
				Request->Callback = std::move(Completion);
				Scheduler = Owner->Resources->Scheduler;
				Owner->Requests.emplace(Request->Id, Request); ++Owner->Active;
				++Owner->Submitting; Submission.Counted = true;
				DispatchReturn.Request = Request;

				{
					std::lock_guard RequestLock(Request->Mutex);
					Request->Group = Private::BeginRequest(RequestOwner, Request);
					Private::FOwnerAccess::TrackDependency(RequestOwner, {
						.IsFinished = [CacheOwner = Request->CacheOwner] { return CacheOwner->Poll(); },
						.Wait = [CacheOwner = Request->CacheOwner] { return CacheOwner->Wait(); },
						.RequestCancel = [CacheOwner = Request->CacheOwner] { CacheOwner->RequestCancel(); },
						.SetPriority = [CacheOwner = Request->CacheOwner](ERequestPriority Priority) { CacheOwner->SetPriority(Priority); }});
					if (Request->Group->IsCanceled()) Request->Cancellation.Cancel();
				}
			}

			if (Request->Cancellation.IsCanceled())
			{
				Request->Run();
				return {};
			}
			const auto& Data = *Request->Data;
			const std::string FunctionName(std::holds_alternative<FBuildDefinition>(Data.Request)
				? std::get<FBuildDefinition>(Data.Request).GetFunctionName() : std::get<FBuildAction>(Data.Request).GetFunction().Name);
			const auto Revision = Request->BeginScheduledStage();
			auto Accepted = Request->Group->LaunchTask(Request.get(), *Scheduler, FunctionName,
				[Request] { Request->Run(); }, [Request] { Request->RequestCancel(); });
			if (Accepted) Request->SetScheduledIfCurrent(std::move(*Accepted), Revision);
			if (!Accepted && Request->RejectDispatch())
			{
				auto Error = std::move(Accepted.error()); Error.BoundDescription();
				return std::unexpected(std::move(Error));
			}
			Request->AcceptDispatch();
			return {};
		}
		catch (...)
		{
			if (Submission.Counted && !Request->RejectDispatch())
			{
				return {};
			}
			return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::InternalFailure, "Build dispatch or admission failed."});
		}
	}
	auto FBuildSession::Close() -> void
	{ State->Close(); }
	auto FBuildSession::Drain() -> EBuildDrainResult
	{ return State->Drain(); }
}
