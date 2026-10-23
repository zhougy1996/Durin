#include "DerivedDataBuildSession.h"
#include "DerivedDataBuildExecutionPrivate.h"
#include "DerivedDataCache/DerivedDataCacheAsync.h"
#include "Logging/LogMacros.h"
#include "RequestOwnerPrivate.h"
#include "CacheDiagnosticsPrivate.h"

namespace Durin::DerivedData
{
	namespace Private
	{
		struct FSessionResources
		{
			FBuildRegistrySnapshot Registry;
			std::shared_ptr<const IBuildInputResolver> Resolver;
			std::shared_ptr<IBuildScheduler> Scheduler;
			FBuildServiceOptions Service;
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
			std::shared_ptr<FSessionResources> Resources;
			std::optional<FBuildCompleteParams> LocalCompletion;
		};
		enum class ERequestState { PendingDispatch, Queued, Running, Completing, Done };
		struct FBuildRequestState : FRequestCompletion, std::enable_shared_from_this<FBuildRequestState>
		{
			FBuildCancellation Cancellation;
			ERequestState State = ERequestState::PendingDispatch;
			uint64 Id = 0;
			std::weak_ptr<FBuildSessionState> Owner;
			std::shared_ptr<FRequestData> Data;
			FBuildCompletionCallback Callback;
			bool ResultReady = false;

			auto Deliver(FBuildCompleteParams Completion) -> void
			{
				const bool bCancelled = Cancellation.IsCancelled();
				auto Session = Owner.lock();
				FRequestOwnerScope Scope(Group.get(), this, Session.get());
				FBuildCompletionCallback Notify;
				{
					std::lock_guard Lock(Mutex);
					if (ResultReady) return;
					if (bCancelled || Cancellation.IsCancellationRequested()) Completion = FBuildCompletionAccess::Canceled(std::move(Completion));
					if (Completion.GetStatus() == EStatus::Canceled) Cancellation.Cancel();
					ResultReady = true; Notify = std::exchange(Callback, {});
				}
				try { if (Notify) Notify(std::move(Completion)); } catch (...) {}
			}

			auto Finish(FBuildCompleteParams Completion) -> void
			{
				const bool bCancelled = Cancellation.IsCancelled();
				auto Session = Owner.lock();
				FBuildCompletionCallback Notify;
				std::shared_ptr<FRequestData> Retired;
				{
					std::lock_guard Lock(Mutex);
					if (State == ERequestState::Completing || State == ERequestState::Done) return;
					State = ERequestState::Completing;
					if (bCancelled || Cancellation.IsCancellationRequested()) Completion = Private::FBuildCompletionAccess::Canceled(std::move(Completion));
					if (Completion.GetStatus() == EStatus::Canceled) Cancellation.Cancel();
					ResultReady = true; Notify = std::exchange(Callback, {});
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
				std::shared_ptr<IBuildScheduledWork> Work;
				{
					std::lock_guard Lock(Mutex);
					if (State == ERequestState::Completing || State == ERequestState::Done) return false;
					Cancellation.Cancel();
					Complete = State == ERequestState::Queued;
					Work = Scheduled;
				}
				if (Complete) Run();
				if (Work) Work->RequestCancel();
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
				Finish(IsCanceled ? FBuildCompleteParams::Canceled(Key, EBuildExecutionFlags::None) : FBuildCompleteParams::Error(Key, EBuildExecutionFlags::None));
			}
			auto StartBuild(std::shared_ptr<FRequestData> Work) -> void
			{
				auto Action = PrepareBuildAction(Work->Request, Work->Resources->Registry,
					Work->Options.InputResolver ? Work->Options.InputResolver : Work->Resources->Resolver, Work->Inputs, Work->Options.Cancellation);
				if (!Action) { Finish(std::move(Action.error())); return; }
				Work->Request = std::move(*Action);
				{ std::lock_guard Lock(Mutex); Data = std::move(Work); }
				const auto Policy = Data->Options.Policy;
				if (!Policy.QueryCache || Policy.ForceBuild) { ResumeLocal(); return; }
				const std::array Requests{FCacheGetRequest{.Key = std::get<FBuildAction>(Data->Request).GetKey(), .OutputLimits = Policy.OutputLimits,
					.MaximumEncodedBytes = Policy.MaximumEncodedBytes, .MaximumWorkingSetBytes = Policy.MaximumWorkingSetBytes,
					.ShouldCancel = [Cancel = Data->Options.Cancellation] { return Cancel.IsCancelled(); }}};
				auto Cache = Data->Resources->Service.AsyncCache; auto CacheOwner = Group;
				auto Self = shared_from_this(); const auto Revision = BeginScheduledStage();
				auto Submitted = Cache->Get(Requests, *CacheOwner,
					[Self](uint32, FCacheGetResult Result, FCacheQueryStats) { Self->ReceiveCache(std::move(Result)); });
				if (!Submitted) ReceiveCache(std::unexpected(std::move(Submitted.error())));
				else SetScheduledIfCurrent(std::move(*Submitted), Revision);
			}
			auto SetScheduledIfCurrent(std::shared_ptr<IBuildScheduledWork> Work, uint64 Revision) -> void
			{
				{ std::lock_guard Lock(Mutex); if (ScheduleRevision != Revision || State == ERequestState::Done) return; Scheduled = Work; StageDispatching = false; }
				if (Cancellation.IsCancelled() && Work) Work->RequestCancel();
				Changed.notify_all();
			}
			auto ReceiveCache(FCacheGetResult Result) -> void
			try
			{
				auto Session = Owner.lock(); FRequestOwnerScope Scope(Group.get(), this, Session.get());
				auto Work = Data;
				const auto Key = std::get<FBuildAction>(Work->Request).GetKey();
				if (Work->Options.Cancellation.IsCancelled()) { Work.reset(); Finish(FBuildCompleteParams::Canceled(Key, EBuildExecutionFlags::None)); return; }
				if (Result && *Result)
				{
					const auto& Function = std::get<FBuildAction>(Work->Request).GetFunction();
					auto Output = (**Result).GetSchema() == Function.OutputType
						&& (**Result).GetSchemaVersion() == Function.OutputSchema
						? (**Result).ToOutput(Key, Work->Options.Policy.OutputLimits)
						: std::expected<FBuildOutput, FCacheError>(std::unexpected(FCacheError{
							ECacheError::Corrupt, "Cache output schema does not match the build function."}));
					if (Output && !Output->HasError()) { Work.reset(); Finish(FBuildCompletionAccess::Ok(std::move(*Output), Key, EBuildExecutionFlags::CacheQueryHit)); return; }
					Private::ObserveCache(Key, (**Result).GetSchema() != Function.OutputType || (**Result).GetSchemaVersion() != Function.OutputSchema
						? Private::ECacheObservation::SchemaRejected : Private::ECacheObservation::RecordRejected);
					Result = std::unexpected(Output ? FCacheError{ECacheError::Corrupt, "Cached build output contains errors."}
						: std::move(Output.error()));
				}
				if (!Result) CacheFailure("read", Result.error());
				if (Work->Options.Policy.BuildLocal) Private::ObserveCache(Key, Private::ECacheObservation::Fallback);
				Work.reset(); ContinueLocal();
			}
			catch (...) { InfrastructureFailure(); }
			auto ContinueLocal() -> void;
			auto StoreLocal(std::shared_ptr<FRequestData> Work, FBuildCompleteParams Completion) -> void
			try
			{
				const auto* Output = Completion.GetOutput();
				if (Completion.GetStatus() != EStatus::Ok || !Output || !Work->Options.Policy.StoreOnBuild
					|| Output->HasLogs()) { Work.reset(); Finish(std::move(Completion)); return; }
				const auto Policy = Work->Options.Policy;
				Work->LocalCompletion = std::move(Completion);
				auto Record = FCacheRecord::FromOutput(*Work->LocalCompletion->GetCacheKey(), *Work->LocalCompletion->GetOutput(), Policy.PersistenceLimits);
				if (Policy.ResultDelivery == EBuildResultDelivery::BeforePersistence)
					Deliver(*Work->LocalCompletion);
				if (!Record) { Work.reset(); ReceiveStore(std::unexpected(std::move(Record.error()))); return; }
				const std::array Requests{FCachePutRequest{std::move(*Record), Policy.MaximumEncodedBytes, Policy.OutputLimits.MaximumTotalBytes, Policy.MaximumWorkingSetBytes,
					[Cancel = Work->Options.Cancellation] { return Cancel.IsCancelled(); }}};
				auto Cache = Work->Resources->Service.AsyncCache; auto CacheOwner = Group; Work.reset();
				auto Self = shared_from_this(); const auto Revision = BeginScheduledStage();
				auto Saved = Cache->Put(Requests, *CacheOwner,
					[Self](uint32, FCachePutResult Result) { Self->ReceiveStore(std::move(Result)); });
				if (!Saved) ReceiveStore(std::unexpected(std::move(Saved.error())));
				else SetScheduledIfCurrent(std::move(*Saved), Revision);
			}
			catch (...) { Work.reset(); ReceiveStore(std::unexpected(FCacheError{ECacheError::StorageFailure, "Cache persistence threw."})); }
			auto ReceiveStore(FCachePutResult Result) -> void
			{
				auto Session = Owner.lock(); FRequestOwnerScope Scope(Group.get(), this, Session.get());
				if (!Data || !Data->LocalCompletion) return;
				if (!Result) CacheFailure("write", Result.error());
				auto Completion = std::move(*Data->LocalCompletion);
				Data->LocalCompletion.reset();
				try { if (Data->Options.Cancellation.IsCancelled()) Completion = FBuildCompletionAccess::Canceled(std::move(Completion)); }
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
					Work->Options.Cancellation = Work->Options.Cancellation.WithObserver([Weak = weak_from_this()] {
						auto Self = Weak.lock(); return !Self || Self->Group->IsCanceled();
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
						Complete = Cancellation.IsCancellationRequested();
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
			if (Work->Options.Cancellation.IsCancelled()) { const auto Key = std::get<FBuildAction>(Work->Request).GetKey(); Work.reset(); Finish(FBuildCompleteParams::Canceled(Key, EBuildExecutionFlags::None)); return; }
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
					Work->Options.InputResolver ? Work->Options.InputResolver : Work->Resources->Resolver, Work->Inputs, Work->Options);
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
			std::shared_ptr<IAsyncCache> InlineCache, TaskCache;
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
			std::shared_ptr<IBuildScheduler> Scheduler, EBuildCacheExecution CacheExecution) -> std::expected<std::shared_ptr<FBuildSession>, FBuildAdmissionError> override
		{
			std::lock_guard Lock(State->Mutex);
			if (State->Closed) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::Closed, "Build service is closed."});
			if (CacheExecution > EBuildCacheExecution::Task)
				return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::InvalidRequest, "Invalid cache execution mode."});
			auto Snapshot = State->Registry.Freeze();
			if (!Snapshot) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::InternalFailure, std::move(Snapshot.error())});
			auto SessionOptions = State->Options;
			if (!Scheduler) Scheduler = CreateInlineBuildScheduler();
			if (!SessionOptions.AsyncCache)
			{
				auto& Cache = CacheExecution == EBuildCacheExecution::Inline ? State->InlineCache : State->TaskCache;
				if (!Cache) Cache = CreateAsyncCache(SessionOptions.Cache,
					CacheExecution == EBuildCacheExecution::Inline ? CreateInlineBuildScheduler()
						: CreateTaskCacheScheduler(),
					SessionOptions.CacheDispatchLimits);
				SessionOptions.AsyncCache = Cache;
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
			std::array<std::shared_ptr<IAsyncCache>, 2> Caches;
			{
				std::lock_guard Lock(State->Mutex);
				State->Closed = true;
				Caches = {State->InlineCache, State->TaskCache};
				for (auto& Weak : State->Sessions) if (auto Session = Weak.lock()) Sessions.push_back(std::move(Session));
			}
			for (const auto& Session : Sessions) Session->Close();
			for (const auto& Cache : Caches) if (Cache) Cache->Close();
		}
		auto Drain() -> EBuildDrainResult override
		{
			Close();
			std::vector<std::shared_ptr<Private::FBuildSessionState>> Sessions;
			std::array<std::shared_ptr<IAsyncCache>, 2> Caches;
			{
				std::lock_guard Lock(State->Mutex);
				Caches = {State->InlineCache, State->TaskCache};
				for (auto& Weak : State->Sessions) if (auto Session = Weak.lock()) Sessions.push_back(std::move(Session));
			}
			for (const auto& Session : Sessions)
				if (Session->Drain() == EBuildDrainResult::WouldBlock) return EBuildDrainResult::WouldBlock;
			for (const auto& Cache : Caches) if (Cache && Cache->Drain() == ERequestWaitResult::WouldBlock)
				return EBuildDrainResult::WouldBlock;
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
	auto FBuildRequest::RequestCancel() const -> bool { return State && State->RequestCancel(); }
	auto FBuildRequest::Wait() const -> ERequestWaitResult
	{ return State ? State->Wait() : ERequestWaitResult::InvalidRequest; }

	auto FBuildRequest::IsResultReady() const -> bool
	{
		if (!State) return false;
		std::lock_guard Lock(State->Mutex); return State->ResultReady;
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
	auto FBuildSession::Build(FBuildDefinition Definition, IRequestOwner& Owner,
		FBuildCompletionCallback Completion, FBuildInputs Inputs, FBuildRequestOptions Options)
		-> std::expected<FBuildRequest, FBuildAdmissionError>
	{ return BuildImpl(std::move(Definition), std::move(Completion), std::move(Inputs), std::move(Options), &Owner); }
	auto FBuildSession::Build(FBuildAction Action, IRequestOwner& Owner,
		FBuildCompletionCallback Completion, FBuildInputs Inputs, FBuildRequestOptions Options)
		-> std::expected<FBuildRequest, FBuildAdmissionError>
	{ return BuildImpl(std::move(Action), std::move(Completion), std::move(Inputs), std::move(Options), &Owner); }
	auto FBuildSession::BuildImpl(std::variant<FBuildDefinition, FBuildAction> BuildRequest,
		FBuildCompletionCallback Completion, FBuildInputs Inputs, FBuildRequestOptions Options,
		IRequestOwner* RequestOwner)
		-> std::expected<FBuildRequest, FBuildAdmissionError>
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
				if (Options.Policy.ResultDelivery > EBuildResultDelivery::BeforePersistence)
					return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::InvalidRequest, "Invalid result delivery policy."});
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
				Request->Cancellation = Request->Data->Options.Cancellation;
				Request->Callback = std::move(Completion);
				Scheduler = Owner->Resources->Scheduler;
				Owner->Requests.emplace(Request->Id, Request); ++Owner->Active;
				++Owner->Submitting; Submission.Counted = true;
				DispatchReturn.Request = Request;
				auto LocalOwner = RequestOwner ? std::shared_ptr<IRequestOwner>{} : Private::CreateRequestOwnerState();
				{
					std::lock_guard RequestLock(Request->Mutex);
					Request->Group = (RequestOwner ? *RequestOwner : *LocalOwner).Begin(Request);
					if (Request->Group->IsCanceled()) Request->Cancellation.Cancel();
				}
			}
			FBuildRequest Handle; Handle.State = Request;
			if (Request->Cancellation.IsCancelled()) { Request->Run(); return Handle; }
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
