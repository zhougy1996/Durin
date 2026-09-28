#include "DerivedDataBuildSession.h"
#include "DerivedDataBuildExecutionPrivate.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <unordered_map>

namespace Durin::DerivedData
{
	namespace Private
	{
		struct FSessionResources
		{
			FBuildRegistrySnapshot Registry;
			std::shared_ptr<const IBuildInputResolver> Resolver;
			FBuildDispatcher Dispatcher;
			FBuildServiceOptions Service;
		};
		struct FExecutionScope;
		thread_local FExecutionScope* CurrentScope = nullptr;
		struct FExecutionScope
		{
			FBuildSessionState* Owner;
			FExecutionScope* Previous = CurrentScope;
			explicit FExecutionScope(FBuildSessionState* Owner) : Owner(Owner) { CurrentScope = this; }
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
			ERequestState State = ERequestState::PendingDispatch;
			bool Abandoned = false;
			uint64 Id = 0;
			std::weak_ptr<FBuildSessionState> Owner;
			std::shared_ptr<FRequestData> Data;
			FBuildCompletionCallback Callback;

			auto Finish(FBuildCompleteParams Completion) -> void
			{
				auto Session = Owner.lock();
				FBuildCompletionCallback Notify;
				std::shared_ptr<FRequestData> Retired;
				{
					std::lock_guard Lock(Mutex);
					if (State == ERequestState::Completing || State == ERequestState::Done) return;
					State = ERequestState::Completing;
					if (Canceled.load()) Completion = FBuildCompleteParams::Canceled(
						Completion.GetCacheKey() ? std::optional(*Completion.GetCacheKey()) : std::nullopt,
						Completion.GetBuildStatus(), Completion.GetReport());
					Notify = std::move(Callback);
					Retired = std::move(Data);
				}
				Retired.reset();
				FExecutionScope Scope(Session.get());
				try { if (Notify) Notify(std::move(Completion)); } catch (...) {}
				Notify = {};
				{ std::lock_guard Lock(Mutex); State = ERequestState::Done; }
				if (Session) Session->Remove(Id);
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
				FExecutionScope Scope(Session.get());
				FBuildCompleteParams Completion = FBuildCompleteParams::Canceled(
					std::nullopt, EBuildStatus::None, {});
				try
				{
					FBuildCancellation Token([&] { return Canceled.load() || Work->Options.Cancellation.IsCancelled(); });
					Completion = ExecuteBuild(Work->Request, Work->Resources->Registry,
						Work->Resources->Resolver, Work->Inputs, Work->Options.Policy,
						Token, Work->Resources->Service);
				}
				catch (...)
				{
					Completion = FBuildCompleteParams::Error({.Reason = EBuildFailureReason::InternalFailure,
						.Operation = EBuildOperation::Build, .Description = "Build callable threw an exception."},
						std::nullopt, EBuildStatus::None, {});
				}
				Work.reset();
				Finish(std::move(Completion));
			}
			auto Drop() -> void
			{
				{ std::lock_guard Lock(Mutex); Abandoned = true; }
				Cancel();
			}
			auto AcceptDispatch() -> void
			{
				bool Complete = false;
				{
					std::lock_guard Lock(Mutex);
					if (State == ERequestState::PendingDispatch)
					{
						State = ERequestState::Queued;
						Complete = Abandoned || Canceled.load();
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
				return true;
			}
		};
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
			~FDispatchTicket() { Request->Drop(); }
		};
		struct FBuildServiceState
		{
			std::mutex Mutex;
			bool Closed = false;
			FBuildRegistry Registry;
			std::optional<FBuildRegistrySnapshot> Snapshot;
			FBuildServiceOptions Options;
			std::vector<std::weak_ptr<FBuildSession>> Sessions;
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
			if (State->Snapshot) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::InvalidRequest, "Build registration is frozen."});
			auto Added = State->Registry.Register(std::move(Function));
			if (!Added) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::InvalidRequest, Added.error().Description});
			return {};
		}
		auto CreateSession(std::shared_ptr<const IBuildInputResolver> Resolver,
			FBuildDispatcher Dispatcher) -> std::expected<std::shared_ptr<FBuildSession>, FBuildAdmissionError> override
		{
			std::lock_guard Lock(State->Mutex);
			if (State->Closed) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::Closed, "Build service is closed."});
			if (!State->Snapshot)
			{
				auto Frozen = State->Registry.Freeze();
				if (!Frozen) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::InternalFailure, Frozen.error().Description});
				State->Snapshot = std::move(*Frozen);
			}
			auto SessionState = std::make_shared<Private::FBuildSessionState>();
			SessionState->Resources = std::make_shared<Private::FSessionResources>(
				*State->Snapshot, std::move(Resolver), std::move(Dispatcher), State->Options);
			auto Session = std::shared_ptr<FBuildSession>(new FBuildSession(std::move(SessionState)));
			State->Sessions.push_back(Session);
			return Session;
		}
		auto Close() -> void override
		{
			std::vector<std::shared_ptr<FBuildSession>> Sessions;
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
			std::vector<std::shared_ptr<FBuildSession>> Sessions;
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

	FBuildSession::FBuildSession(std::shared_ptr<Private::FBuildSessionState> InState) : State(std::move(InState)) {}
	FBuildSession::~FBuildSession() { Close(); Drain(); }
	auto FBuildRequest::Cancel() const -> bool { return State && State->Cancel(); }
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
	auto FBuildSession::BuildImpl(std::variant<FBuildDefinition, FBuildAction> BuildRequest,
		FBuildCompletionCallback Completion, FBuildInputs Inputs, FBuildRequestOptions Options)
		-> std::expected<FBuildRequest, FBuildAdmissionError>
	{
		auto Owner = State;
		Private::FSubmissionScope Submission{Owner};
		Private::FExecutionScope Execution(Owner.get());
		std::shared_ptr<Private::FBuildRequestState> Request;
		try
		{
			FBuildDispatcher Dispatch;
			{
				std::lock_guard Lock(Owner->Mutex);
				if (Owner->Closed) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::Closed, "Build session is closed."});
				if (!Completion) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::InvalidRequest, "Build completion callback is missing."});
				if (Owner->Active >= 4096) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::Capacity, "Build session is full."});
				const std::string_view Name = std::holds_alternative<FBuildDefinition>(BuildRequest)
					? std::get<FBuildDefinition>(BuildRequest).GetFunctionName() : std::get<FBuildAction>(BuildRequest).GetFunction().Name;
				if (!Owner->Resources->Registry.Find(Name))
					return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::MissingFunction, "Build function is not registered."});
				Request = std::make_shared<Private::FBuildRequestState>();
				Request->Id = Owner->NextId++;
				Request->Owner = Owner;
				Request->Data = std::make_shared<Private::FRequestData>(
					std::move(BuildRequest), std::move(Inputs), std::move(Options), Owner->Resources);
				Request->Callback = std::move(Completion);
				Dispatch = Owner->Resources->Dispatcher;
				Owner->Requests.emplace(Request->Id, Request); ++Owner->Active;
				++Owner->Submitting; Submission.Counted = true;
			}
			FBuildRequest Handle; Handle.State = Request;
			if (!Dispatch) { Request->Run(); return Handle; }
			auto Ticket = std::make_shared<Private::FDispatchTicket>(); Ticket->Request = Request;
			auto Accepted = Dispatch([Ticket] { Ticket->Request->Run(); });
			Ticket.reset();
			if (!Accepted && Request->RejectDispatch()) return std::unexpected(std::move(Accepted.error()));
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
	{
		auto Owner = State;
		decltype(Owner->Requests) Pending;
		{
			std::lock_guard Lock(Owner->Mutex);
			if (Owner->Closed) return;
			Owner->Closed = true; Pending = std::move(Owner->Requests);
		}
		for (const auto& [Id, Request] : Pending) Request->Cancel();
		Owner->ReleaseIfDrained();
	}
	auto FBuildSession::Drain() -> EBuildDrainResult
	{
		auto Owner = State;
		Close();
		for (auto* Scope = Private::CurrentScope; Scope; Scope = Scope->Previous)
			if (Scope->Owner == Owner.get()) return EBuildDrainResult::WouldBlock;
		std::unique_lock Lock(Owner->Mutex);
		Owner->Changed.wait(Lock, [&] { return Owner->Active == 0 && Owner->Submitting == 0 && Owner->Released; });
		return EBuildDrainResult::Drained;
	}
}
