#include "BuildCacheRecordPrivate.h"
#include "DerivedDataBuildSession.h"

#include "DerivedDataBuildExecutionPrivate.h"
#include "CacheDispatchPrivate.h"
#include "BuildServiceBackendPrivate.h"
#include "RequestOwnerPrivate.h"
#include "CacheDiagnosticsPrivate.h"
#include "DerivedDataCache/DerivedDataCache.h"

namespace Durin::DerivedData
{
	namespace Private
	{
		struct FBuildSessionState
		{
			std::string Name;
			FBuildRegistrySnapshot Registry;
			std::shared_ptr<const IBuildInputResolver> Resolver;
			std::shared_ptr<IBuildScheduler> Scheduler;
			std::shared_ptr<ICache> Cache;
			FBuildRequestLimits Limits;
		};
		struct FRequestData
		{
			std::variant<FBuildDefinition, FBuildAction> Request;
			FBuildInputs Inputs;
			FBuildPolicy Policy;
			std::optional<FBuildCompleteParams> LocalCompletion;
		};
		struct FBuildRequestState : std::enable_shared_from_this<FBuildRequestState>
		{
			explicit FBuildRequestState(IRequestOwner& Owner) : Group(Owner) {}
			IRequestOwner& Group;
			std::mutex Mutex;
			std::shared_ptr<FBuildSessionState> Session;
			std::shared_ptr<FRequestData> Data;
			FBuildCompletionCallback Callback;
			auto GetData() -> std::shared_ptr<FRequestData> { std::lock_guard Lock(Mutex); return Data; }
			auto Finish(FBuildCompleteParams Completion) -> void
			{
				FBuildCompletionCallback Notify;
				std::shared_ptr<FRequestData> Retired;
				{ std::lock_guard Lock(Mutex); if (!Callback) return;
					if (Group.IsCanceled()) Completion = FBuildCompletionAccess::Canceled(std::move(Completion));
					Notify = std::exchange(Callback, {}); Retired = std::move(Data);
				}
				Retired.reset();
				try { Notify(std::move(Completion)); } catch (...) {}
			}
			auto Fail(std::string Diagnostic, EStatus Status = EStatus::Error) -> void
			{
				auto Work = GetData(); std::optional<FCacheKey> Key;
				if (Work) if (const auto* Action = std::get_if<FBuildAction>(&Work->Request)) Key = Action->GetKey();
				Work.reset(); Finish(Status == EStatus::Canceled ? FBuildCompleteParams::Canceled(Key, EBuildExecutionFlags::None) : FBuildCompleteParams::Error(Key, EBuildExecutionFlags::None, std::move(Diagnostic)));
			}
			auto Start() -> void
			{
				if (Group.IsCanceled()) { Finish(FBuildCompleteParams::Canceled({}, EBuildExecutionFlags::None)); return; }
				auto Work = GetData(); if (!Work) return;
				if (std::holds_alternative<FBuildAction>(Work->Request)) { Prepare({}); return; }
				const auto& Definition = std::get<FBuildDefinition>(Work->Request);
				if (Work->Inputs.IsValid()) {
					std::vector<FBuildInputReference> Identities;
					for (const auto& Input : Work->Inputs.GetInputs()) Identities.push_back(Input.Identity);
					Prepare(std::move(Identities)); return;
				}
				if (Definition.GetSources().empty()) { Prepare({}); return; }
				if (!Session->Resolver) { Work.reset(); Fail("No build input resolver is available."); return; }
				Session->Resolver->ResolveInputMeta(Definition, Group, [Self = shared_from_this()](FBuildInputMetaResolvedParams&& Params) {
					try {
						if (Params.Status != EStatus::Ok) { Self->Fail("Build input metadata resolution failed.", Params.Status); return; }
						Self->Prepare(std::move(Params.Inputs));
					} catch (...) { Self->Fail("Build input metadata completion failed."); }
				});
			}
			auto Prepare(std::vector<FBuildInputReference> Identities) -> void
			{
				auto Work = GetData(); if (!Work) return;
				auto Action = PrepareBuildAction(Work->Request, Session->Registry, Identities);
				if (!Action) { auto Diagnostic = std::move(Action.error()); Work.reset(); Fail(std::move(Diagnostic)); return; }
				Work->Request = std::move(*Action);
				if (!Work->Policy.Has(EBuildPolicy::CacheQuery)) { Work.reset(); ResolveData(); return; }
				const auto& Prepared = std::get<FBuildAction>(Work->Request);
				const std::array Requests{FCacheGetRequest{.Key = Prepared.GetKey(), .Name = Prepared.GetFunction().Name}};
				Session->Cache->Get(Requests, Group, [Self = shared_from_this()](FCacheGetResponse Response) {
					try { Self->ReceiveCache(std::move(Response)); } catch (...) { Self->Fail("Cache response failed."); }
				});
			}
			auto ReceiveCache(FCacheGetResponse Response) -> void
			{
				auto Work = GetData(); if (!Work) return;
				const auto& Action = std::get<FBuildAction>(Work->Request);
				if (Group.IsCanceled() || Response.Status == EStatus::Canceled) { const auto Key = Action.GetKey(); Work.reset(); Finish(FBuildCompleteParams::Canceled(Key, EBuildExecutionFlags::None)); return; }
				if (Response.Status == EStatus::Ok) {
					auto Output = ReadBuildOutput(Action.GetKey(), Response.Record, Session->Limits.Output);
					const bool SchemaMismatch = Output && (Output->GetSchema() != Action.GetFunction().OutputType || Output->GetSchemaVersion() != Action.GetFunction().OutputSchema);
					if (Output && !SchemaMismatch && !Output->HasError()) { const auto Key = Action.GetKey(); Work.reset(); Finish(FBuildCompletionAccess::Ok(std::move(*Output), Key, EBuildExecutionFlags::CacheQueryHit)); return; }
					ObserveCache(Action.GetKey(), SchemaMismatch ? ECacheObservation::SchemaRejected : ECacheObservation::RecordRejected);
				}
				if (Work->Policy.Has(EBuildPolicy::BuildLocal)) ObserveCache(Action.GetKey(), ECacheObservation::Fallback);
				Work.reset(); ResolveData();
			}
			auto ResolveData() -> void
			{
				auto Work = GetData(); if (!Work) return;
				if (Group.IsCanceled()) { const auto Key = std::get<FBuildAction>(Work->Request).GetKey(); Work.reset(); Finish(FBuildCompleteParams::Canceled(Key, EBuildExecutionFlags::None)); return; }
				if (!Work->Policy.Has(EBuildPolicy::BuildLocal)) { Work.reset(); Fail("No usable cache value is available and local build is disabled."); return; }
				const auto& Action = std::get<FBuildAction>(Work->Request);
				if (Work->Inputs.IsValid() || Action.GetInputs().empty()) { Work.reset(); ScheduleLocal(); return; }
				if (!Session->Resolver) { Work.reset(); Fail("Build input payloads are unavailable."); return; }
				Session->Resolver->ResolveInputData(Action, Group, [Self = shared_from_this()](FBuildInputDataResolvedParams&& Params) {
					try {
						if (Params.Status != EStatus::Ok) { Self->Fail("Build input data resolution failed.", Params.Status); return; }
						auto Work = Self->GetData(); if (!Work) return;
						FBuildInputsBuilder Builder; for (auto& Input : Params.Inputs) Builder.AddInput(std::move(Input));
						Work->Inputs = std::move(Builder).Build(); Work.reset(); Self->ScheduleLocal();
					} catch (...) { Self->Fail("Build input data completion failed."); }
				});
			}
			auto ScheduleLocal() -> void
			{
				auto Work = GetData(); if (!Work) return;
				const auto Name = std::get<FBuildAction>(Work->Request).GetFunction().Name;
				FOwnerAccess::LaunchTask(Group, *Session->Scheduler, Name, [Self = shared_from_this()] { Self->RunLocal(); }, [Self = shared_from_this()] { Self->Fail("Build dispatch was dropped."); });
			}
			auto RunLocal() -> void
			{
				auto Work = GetData(); if (!Work) return;
				try {
					const auto Inputs = Work->Inputs.GetInputs(); std::vector<FBuildInput> Values(Inputs.begin(), Inputs.end()); Work->Inputs = {};
					auto Completion = ExecuteBuildLocal(std::get<FBuildAction>(Work->Request), Session->Registry, std::move(Values), Work->Policy, Session->Limits, Group);
					const auto* Output = &Completion.Output;
					if (Completion.Status != EStatus::Ok || !Work->Policy.Has(EBuildPolicy::CacheStoreOnBuild) || Output->HasLogs()) { Work.reset(); Finish(std::move(Completion)); return; }
					Work->LocalCompletion = std::move(Completion);
					auto Record = MakeBuildCacheRecord(Work->LocalCompletion->CacheKey, Work->LocalCompletion->Output, Session->Limits.Output);
					if (!Record) { Work.reset(); FinishStore(); return; }
					const std::array Requests{FCachePutRequest{.Record = std::move(*Record), .Policy = ECachePolicy::Store, .Name = std::get<FBuildAction>(Work->Request).GetFunction().Name}};
					Work.reset(); Session->Cache->Put(Requests, Group, [Self = shared_from_this()](FCachePutResponse Response) { Self->FinishStore(Response.Status); });
				} catch (...) { Work.reset(); Fail("Local build execution failed."); }
			}
			auto FinishStore(EStatus Status = EStatus::Ok) -> void
			{
				auto Work = GetData(); if (!Work || !Work->LocalCompletion) return;
				auto Completion = std::move(*Work->LocalCompletion); Work->LocalCompletion.reset();
				if (Status == EStatus::Canceled) Completion = FBuildCompletionAccess::Canceled(std::move(Completion));
				Work.reset(); Finish(std::move(Completion));
			}
		};
		struct FBuildServiceState
		{
			std::mutex Mutex;
			FBuildRegistry Registry;
			FBuildServiceOptions Options;
			std::shared_ptr<ICache> Cache;
			std::shared_ptr<ICacheBackend> Backend;
		};
	}

	class FBuildService final : public IBuild
	{
	public:
		explicit FBuildService(FBuildServiceOptions Options, std::shared_ptr<ICacheBackend> Backend = {}) : State(std::make_shared<Private::FBuildServiceState>())
		{ State->Options = std::move(Options); State->Backend = std::move(Backend); }
		auto Register(std::shared_ptr<const IBuildFunction> Function)
			-> std::expected<void, std::string> override
		{
			std::lock_guard Lock(State->Mutex);
			return State->Registry.Register(std::move(Function));
		}
		auto CreateSession(std::string Name, std::shared_ptr<const IBuildInputResolver> Resolver,
			std::shared_ptr<IBuildScheduler> Scheduler) -> FBuildSession override
		{
			std::lock_guard Lock(State->Mutex);
			auto Snapshot = State->Registry.Freeze();
			auto SessionCache = State->Options.Cache;
			if (!Scheduler) Scheduler = CreateTaskBuildScheduler();
			if (!SessionCache) {
				auto& Cache = State->Cache;
				if (!Cache) Cache = Private::CreateCacheForBackend(State->Backend,
					CreateTaskCacheScheduler(),
					State->Options.CacheDispatchLimits, State->Options.CacheLimits);
				SessionCache = Cache;
			}
			auto SessionState = std::make_shared<Private::FBuildSessionState>();
			SessionState->Name = std::move(Name);
			SessionState->Limits = State->Options.BuildLimits;
			SessionState->Registry = std::move(Snapshot);
			SessionState->Resolver = std::move(Resolver);
			SessionState->Scheduler = std::move(Scheduler);
			SessionState->Cache = std::move(SessionCache);
			return FBuildSession(std::move(SessionState));
		}
	private:
		std::shared_ptr<Private::FBuildServiceState> State;
	};

	auto CreateBuild(FBuildServiceOptions Options) -> std::shared_ptr<IBuild>
	{ return std::make_shared<FBuildService>(std::move(Options)); }
	auto Private::CreateBuildForBackend(FBuildBackendOptions Options) -> std::shared_ptr<IBuild>
	{
		return std::make_shared<FBuildService>(FBuildServiceOptions{.BuildLimits = Options.BuildLimits.value_or(FBuildRequestLimits{}), .CacheDispatchLimits = Options.CacheDispatchLimits, .CacheLimits = Options.CacheLimits}, std::move(Options.Cache));
	}
	auto GetBuild() -> IBuild&
	{
		static const std::shared_ptr<IBuild> Build = CreateBuild();
		return *Build;
	}

	FBuildSession::FBuildSession(std::shared_ptr<Private::FBuildSessionState> InState) : State(std::move(InState)) {}
	FBuildSession::~FBuildSession() = default;
	auto FBuildSession::Build(const FBuildDefinition& Definition, const FBuildInputs& Inputs, const FBuildPolicy& Policy, IRequestOwner& Owner, FBuildCompletionCallback&& Completion) -> void
	{ BuildImpl(Definition, std::move(Completion), Inputs, Policy, Owner); }
	auto FBuildSession::Build(const FBuildAction& Action, const FBuildInputs& Inputs, const FBuildPolicy& Policy, IRequestOwner& Owner, FBuildCompletionCallback&& Completion) -> void
	{ BuildImpl(Action, std::move(Completion), Inputs, Policy, Owner); }
	auto FBuildSession::BuildImpl(std::variant<FBuildDefinition, FBuildAction> BuildRequest, FBuildCompletionCallback Completion, FBuildInputs Inputs, FBuildPolicy Policy, IRequestOwner& RequestOwner) -> void
	{
		if (!Completion) Completion = [](FBuildCompleteParams&&) {};
		FRequestBarrier Barrier(RequestOwner);
		auto Owner = State;
		auto Request = std::make_shared<Private::FBuildRequestState>(RequestOwner);
		Request->Callback = std::move(Completion);
		try {
			Request->Session = Owner;
			auto Fail = [&](std::string Diagnostic) { Request->Finish(FBuildCompleteParams::Error({}, EBuildExecutionFlags::None, std::move(Diagnostic))); };
			if ((Policy.GetCombinedPolicy() & ~EBuildPolicy::Default) != EBuildPolicy::None) { Fail("Unsupported build policy flags."); return; }
			if (RequestOwner.IsCanceled()) { Request->Finish(FBuildCompleteParams::Canceled({}, EBuildExecutionFlags::None)); return; }
			Request->Data = std::make_shared<Private::FRequestData>(std::move(BuildRequest), std::move(Inputs), Policy);
			auto Scheduler = Owner->Scheduler;
			const auto& Data = *Request->Data;
			const std::string FunctionName(std::holds_alternative<FBuildDefinition>(Data.Request) ? std::get<FBuildDefinition>(Data.Request).GetFunctionName() : std::get<FBuildAction>(Data.Request).GetFunction().Name);
			Private::FOwnerAccess::LaunchTask(Request->Group, *Scheduler, FunctionName,
				[Request] { Request->Start(); }, [Request] { Request->Fail("Build dispatch was dropped."); });
		}
		catch (...) { Request->Fail("Build submission failed."); }
	}
	auto FBuildSession::GetName() const -> std::string_view { return State->Name; }
}
