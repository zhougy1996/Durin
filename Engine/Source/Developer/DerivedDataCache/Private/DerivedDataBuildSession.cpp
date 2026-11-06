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
		struct FBuildRequestState : std::enable_shared_from_this<FBuildRequestState>
		{
			FBuildRequestState(IRequestOwner& Owner, std::variant<FBuildDefinition, FBuildAction> InRequest, FBuildInputs InInputs, FBuildPolicy InPolicy)
				: Group(Owner), Inputs(std::move(InInputs)), Policy(InPolicy)
			{
				if (auto* Value = std::get_if<FBuildDefinition>(&InRequest)) Definition = std::move(*Value);
				else { Action = std::get<FBuildAction>(std::move(InRequest)); CacheKey = Action->GetKey(); }
			}
			IRequestOwner& Group;
			std::mutex Mutex;
			std::shared_ptr<FBuildSessionState> Session;
			std::optional<FBuildDefinition> Definition;
			std::optional<FBuildAction> Action;
			FBuildInputs Inputs;
			FBuildPolicy Policy;
			FCacheKey CacheKey; // Completion identity survives retiring action metadata.
			std::optional<FBuildCompleteParams> LocalCompletion;
			FBuildCompletionCallback Callback;
			auto Finish(FBuildCompleteParams Completion) -> void
			{
				FBuildCompletionCallback Notify;
				{
					std::optional<FBuildDefinition> RetiredDefinition;
					std::optional<FBuildAction> RetiredAction;
					FBuildInputs RetiredInputs;
					std::optional<FBuildCompleteParams> RetiredCompletion;
					{ std::lock_guard Lock(Mutex); if (!Callback) return;
						if (Group.IsCanceled() || Completion.Status == EStatus::Canceled) Completion = FBuildCompleteParams::Canceled(Completion.CacheKey, Completion.BuildStatus, std::move(Completion.Output));
						Notify = std::exchange(Callback, {});
						RetiredDefinition = std::exchange(Definition, {});
						RetiredAction = std::exchange(Action, {});
						RetiredInputs = std::exchange(Inputs, {});
						RetiredCompletion = std::exchange(LocalCompletion, {});
					}
				}
				try { Notify(std::move(Completion)); } catch (...) {}
			}
			auto Fail(std::string Diagnostic, EStatus Status = EStatus::Error) -> void
			{
				FCacheKey Key;
				{ std::lock_guard Lock(Mutex); if (!Callback) return;
					Key = CacheKey;
				}
				Finish(Status == EStatus::Canceled ? FBuildCompleteParams::Canceled(Key, EBuildExecutionFlags::None) : FBuildCompleteParams::Error(Key, EBuildExecutionFlags::None, std::move(Diagnostic)));
			}
			auto Start() -> void
			{
				if (Group.IsCanceled()) { Finish(FBuildCompleteParams::Canceled({}, EBuildExecutionFlags::None)); return; }
				FBuildInputs Captured;
				{ std::lock_guard Lock(Mutex); if (!Callback) return; Captured = Inputs; }
				if (Action) { Prepare({}); return; }
				if (Captured.IsValid()) {
					std::vector<FBuildInputReference> Identities;
					for (const auto& Input : Captured.GetInputs()) Identities.push_back(Input.Identity);
					Captured = {}; Prepare(std::move(Identities)); return;
				}
				if (Definition->GetSources().empty()) { Prepare({}); return; }
				if (!Session->Resolver) { Fail("No build input resolver is available."); return; }
				Session->Resolver->ResolveInputMeta(*Definition, Group, [Self = shared_from_this()](FBuildInputMetaResolvedParams&& Params) {
					try {
						if (Params.Status != EStatus::Ok) { Self->Fail("Build input metadata resolution failed.", Params.Status); return; }
						Self->Prepare(std::move(Params.Inputs));
					} catch (...) { Self->Fail("Build input metadata completion failed."); }
				});
			}
			auto Prepare(std::vector<FBuildInputReference> Identities) -> void
			{
				{ std::lock_guard Lock(Mutex); if (!Callback) return; }
				if (Action) {
					const auto Entry = Session->Registry.Find(Action->GetFunction().Name);
					if (!Entry) { Fail("Build function is not registered."); return; }
					if (Action->GetFunction() != Entry->Descriptor) { Fail("Build action does not match the registered function."); return; }
				}
				else {
					auto Prepared = PrepareBuildAction(*Definition, Session->Registry, Identities);
					if (!Prepared) { Fail(std::move(Prepared.error())); return; }
					{ std::lock_guard Lock(Mutex); if (!Callback) return; Action = std::move(*Prepared); CacheKey = Action->GetKey(); }
				}
				if (!Policy.Has(EBuildPolicy::CacheQuery)) { ResolveData(); return; }
				const std::array Requests{FCacheGetRequest{.Key = Action->GetKey(), .Name = Action->GetFunction().Name}};
				Session->Cache->Get(Requests, Group, [Self = shared_from_this()](FCacheGetResponse Response) {
					try { Self->ReceiveCache(std::move(Response)); } catch (...) { Self->Fail("Cache response failed."); }
				});
			}
			auto ReceiveCache(FCacheGetResponse Response) -> void
			{
				{ std::lock_guard Lock(Mutex); if (!Callback) return; }
				if (Group.IsCanceled() || Response.Status == EStatus::Canceled) { Finish(FBuildCompleteParams::Canceled(Action->GetKey(), EBuildExecutionFlags::None)); return; }
				if (Response.Status == EStatus::Ok) {
					auto Output = ReadBuildOutput(Action->GetKey(), Response.Record, Session->Limits.Output);
					const bool SchemaMismatch = Output && (Output->GetSchema() != Action->GetFunction().OutputType || Output->GetSchemaVersion() != Action->GetFunction().OutputSchema);
					if (Output && !SchemaMismatch && !Output->HasError()) { Finish(FBuildCompleteParams{Action->GetKey(), std::move(*Output), EBuildExecutionFlags::CacheQueryHit, EStatus::Ok}); return; }
					ObserveCache(Action->GetKey(), SchemaMismatch ? ECacheObservation::SchemaRejected : ECacheObservation::RecordRejected);
				}
				if (Policy.Has(EBuildPolicy::BuildLocal)) ObserveCache(Action->GetKey(), ECacheObservation::Fallback);
				ResolveData();
			}
			auto ResolveData() -> void
			{
				bool HasInputs;
				{ std::lock_guard Lock(Mutex); if (!Callback) return; HasInputs = Inputs.IsValid(); }
				if (Group.IsCanceled()) { Finish(FBuildCompleteParams::Canceled(Action->GetKey(), EBuildExecutionFlags::None)); return; }
				if (!Policy.Has(EBuildPolicy::BuildLocal)) { Fail("No usable cache value is available and local build is disabled."); return; }
				if (HasInputs || Action->GetInputs().empty()) { ScheduleLocal(); return; }
				if (!Session->Resolver) { Fail("Build input payloads are unavailable."); return; }
				Session->Resolver->ResolveInputData(*Action, Group, [Self = shared_from_this()](FBuildInputDataResolvedParams&& Params) {
					try {
						if (Params.Status != EStatus::Ok) { Self->Fail("Build input data resolution failed.", Params.Status); return; }
						FBuildInputsBuilder Builder; for (auto& Input : Params.Inputs) Builder.AddInput(std::move(Input));
						{ std::lock_guard Lock(Self->Mutex); if (!Self->Callback) return; Self->Inputs = std::move(Builder).Build(); }
						Self->ScheduleLocal();
					} catch (...) { Self->Fail("Build input data completion failed."); }
				});
			}
			auto ScheduleLocal() -> void
			{
				std::string Name;
				{ std::lock_guard Lock(Mutex); if (!Callback) return; Name = Action->GetFunction().Name; }
				FOwnerAccess::LaunchTask(Group, *Session->Scheduler, Name, [Self = shared_from_this()] { Self->RunLocal(); }, [Self = shared_from_this()] { Self->Fail("Build dispatch was dropped."); });
			}
			auto RunLocal() -> void
			{
				FBuildInputs Resolved;
				std::optional<FBuildAction> LocalAction;
				{ std::lock_guard Lock(Mutex); if (!Callback) return; Resolved = std::exchange(Inputs, {}); LocalAction = std::exchange(Action, {}); }
				const auto Key = CacheKey;
				try {
					const auto View = Resolved.GetInputs(); std::vector<FBuildInput> Values(View.begin(), View.end()); Resolved = {};
					auto Completion = ExecuteBuildLocal(*LocalAction, Session->Registry, std::move(Values), Policy, Session->Limits, Group);
					if (Completion.Status != EStatus::Ok || !Policy.Has(EBuildPolicy::CacheStoreOnBuild) || Completion.Output.HasLogs()) { LocalAction.reset(); Finish(std::move(Completion)); return; }
					auto Record = MakeBuildCacheRecord(Completion.CacheKey, Completion.Output, Session->Limits.Output);
					if (!Record) { LocalAction.reset(); Finish(std::move(Completion)); return; }
					const std::string Name(LocalAction->GetFunction().Name); LocalAction.reset();
					{ std::lock_guard Lock(Mutex); if (!Callback) return; LocalCompletion = std::move(Completion); Definition.reset(); }
					const std::array Requests{FCachePutRequest{.Record = std::move(*Record), .Policy = ECachePolicy::Store, .Name = Name}};
					Session->Cache->Put(Requests, Group, [Self = shared_from_this()](FCachePutResponse Response) { Self->FinishStore(Response.Status); });
				} catch (...) { Finish(FBuildCompleteParams::Error(Key, EBuildExecutionFlags::None, "Local build execution failed.")); }
			}
			auto FinishStore(EStatus Status) -> void
			{
				std::optional<FBuildCompleteParams> Completion;
				{ std::lock_guard Lock(Mutex); if (!Callback || !LocalCompletion) return; Completion = std::exchange(LocalCompletion, {}); }
				if (Status == EStatus::Canceled) Completion->Status = EStatus::Canceled;
				Finish(std::move(*Completion));
			}
		};
	}

	namespace Private
	{
		class FBuild final : public IBuild
		{
		public:
			explicit FBuild(FBuildServiceOptions InOptions, std::shared_ptr<ICacheBackend> InBackend = {})
				: Options(std::move(InOptions)), Backend(std::move(InBackend)) {}
			auto Register(std::shared_ptr<const IBuildFunction> Function)
				-> std::expected<void, std::string> override
			{
				std::lock_guard Lock(Mutex);
				return Registry.Register(std::move(Function));
			}
			auto CreateSession(std::string Name, std::shared_ptr<const IBuildInputResolver> Resolver,
				std::shared_ptr<IBuildScheduler> Scheduler) -> FBuildSession override
			{
				std::lock_guard Lock(Mutex);
				auto Snapshot = Registry.Freeze();
				auto SessionCache = Options.Cache;
				if (!Scheduler) Scheduler = CreateTaskBuildScheduler();
				if (!SessionCache) {
					if (!Cache) Cache = Private::CreateCacheForBackend(Backend,
						CreateTaskCacheScheduler(),
						Options.CacheDispatchLimits, Options.CacheLimits);
					SessionCache = Cache;
				}
				auto SessionState = std::make_shared<Private::FBuildSessionState>();
				SessionState->Name = std::move(Name);
				SessionState->Limits = Options.BuildLimits;
				SessionState->Registry = std::move(Snapshot);
				SessionState->Resolver = std::move(Resolver);
				SessionState->Scheduler = std::move(Scheduler);
				SessionState->Cache = std::move(SessionCache);
				return FBuildSession(std::move(SessionState));
			}
		private:
			std::mutex Mutex;
			FBuildRegistry Registry;
			FBuildServiceOptions Options;
			std::shared_ptr<ICache> Cache;
			std::shared_ptr<ICacheBackend> Backend;
		};
	}

	auto CreateBuild(FBuildServiceOptions Options) -> std::shared_ptr<IBuild>
	{ return std::make_shared<Private::FBuild>(std::move(Options)); }
	auto Private::CreateBuildForBackend(FBuildBackendOptions Options) -> std::shared_ptr<IBuild>
	{
		return std::make_shared<Private::FBuild>(FBuildServiceOptions{.BuildLimits = Options.BuildLimits.value_or(FBuildRequestLimits{}), .CacheDispatchLimits = Options.CacheDispatchLimits, .CacheLimits = Options.CacheLimits}, std::move(Options.Cache));
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
		FRequestBarrier Barrier(RequestOwner, ERequestBarrierFlags::Priority);
		auto Owner = State;
		auto Request = std::make_shared<Private::FBuildRequestState>(RequestOwner, std::move(BuildRequest), std::move(Inputs), Policy);
		Request->Callback = std::move(Completion);
		try {
			Request->Session = Owner;
			auto Fail = [&](std::string Diagnostic) { Request->Finish(FBuildCompleteParams::Error({}, EBuildExecutionFlags::None, std::move(Diagnostic))); };
			if ((Policy.GetCombinedPolicy() & ~EBuildPolicy::Default) != EBuildPolicy::None) { Fail("Unsupported build policy flags."); return; }
			if (RequestOwner.IsCanceled()) { Request->Finish(FBuildCompleteParams::Canceled({}, EBuildExecutionFlags::None)); return; }
			auto Scheduler = Owner->Scheduler;
			const auto& Data = *Request;
			const std::string FunctionName(Data.Definition ? Data.Definition->GetFunctionName() : Data.Action->GetFunction().Name);
			Private::FOwnerAccess::LaunchTask(Request->Group, *Scheduler, FunctionName,
				[Request] { try { Request->Start(); } catch (...) { Request->Fail("Build execution failed."); } }, [Request] { Request->Fail("Build dispatch was dropped."); });
		}
		catch (...) { Request->Fail("Build submission failed."); }
	}
	auto FBuildSession::GetName() const -> std::string_view { return State->Name; }
}
