#include "DerivedDataBuildSession.h"
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
				Retired.reset(); // Destroy callable owners outside session locks.
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
			FBuildDefinition Definition;
			FBuildRequestOptions Options;
			std::shared_ptr<FSessionResources> Resources;
		};
		enum class ERequestState { PendingDispatch, Queued, Running, Completing, Done };
		struct FBuildRequestState
		{
			std::mutex Mutex;
			std::atomic<bool> Cancelled = false;
			ERequestState Phase = ERequestState::PendingDispatch;
			bool Abandoned = false;
			uint64 Id = 0;
			std::weak_ptr<FBuildSessionState> Owner;
			std::shared_ptr<FRequestData> Data;
			FBuildCompletionCallback Callback;

			auto Finish(FBuildResult Result) -> void
			{
				auto Session = Owner.lock();
				FBuildCompletionCallback Completion;
				std::shared_ptr<FRequestData> Retired;
				{
					std::lock_guard Lock(Mutex);
					if (Phase == ERequestState::Completing || Phase == ERequestState::Done) return;
					Phase = ERequestState::Completing;
					if (Cancelled.load()) Result = std::unexpected(FBuildError{
						.Category = EBuildErrorCategory::Cancelled});
					Completion = std::move(Callback); Retired = std::move(Data);
				}
				Retired.reset();
				FExecutionScope Scope(Session.get());
				try { if (Completion) Completion(std::move(Result)); } catch (...) {} // Terminal accounting survives callback failure.
				Completion = {};
				{ std::lock_guard Lock(Mutex); Phase = ERequestState::Done; }
				if (Session) Session->Remove(Id);
			}
			auto Cancel() -> bool
			{
				bool Complete = false;
				{
					std::lock_guard Lock(Mutex);
					if (Phase == ERequestState::Completing || Phase == ERequestState::Done) return false;
					Cancelled.store(true);
					Complete = Phase == ERequestState::Queued;
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
					if (Phase != ERequestState::PendingDispatch && Phase != ERequestState::Queued) return;
					Phase = ERequestState::Running; Work = std::move(Data);
				}
				FExecutionScope Scope(Session.get());
				FBuildResult Result = std::unexpected(FBuildError{
					.Category = EBuildErrorCategory::Unavailable});
				try
				{
					FBuildCancellation Token([&] { return Cancelled.load() || Work->Options.Cancellation.IsCancelled(); });
					Result = ExecuteBuildRequest(Work->Definition, Work->Resources->Registry, *Work->Resources->Resolver,
						Work->Options.Policy, Token, Work->Options.Cache, Work->Options.Observer);
				}
				catch (...)
				{
					Result = std::unexpected(FBuildError{.Phase = EBuildSessionPhase::Build,
						.Category = EBuildErrorCategory::ProducerFailure, .Description = "Build callable threw an exception."});
				}
				Work.reset(); // Release resolver, services and observers before terminal accounting.
				Finish(std::move(Result));
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
					if (Phase == ERequestState::PendingDispatch)
					{
						Phase = ERequestState::Queued;
						Complete = Abandoned || Cancelled.load();
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
					if (Phase != ERequestState::PendingDispatch) return false; // An inline invocation already admitted it.
					Phase = ERequestState::Done; Retired = std::move(Data); RetiredCallback = std::move(Callback);
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
	}

	FBuildSession::FBuildSession(FBuildRegistrySnapshot Registry, std::shared_ptr<const IBuildInputResolver> Resolver, FBuildDispatcher Dispatcher)
		: State(std::make_shared<Private::FBuildSessionState>())
	{
		State->Resources = std::make_shared<Private::FSessionResources>(std::move(Registry), std::move(Resolver), std::move(Dispatcher));
	}
	FBuildSession::~FBuildSession() { Close(); Drain(); }

	auto FBuildRequest::Cancel() const -> bool { return State && State->Cancel(); }
	auto FBuildRequest::IsComplete() const -> bool
	{
		if (!State) return false;
		std::lock_guard Lock(State->Mutex); return State->Phase == Private::ERequestState::Done;
	}
	auto FBuildSession::Submit(FBuildDefinition Definition, FBuildCompletionCallback Completion, FBuildRequestOptions Options)
		-> std::expected<FBuildRequest, FBuildError>
	{ return SubmitImpl(std::move(Definition), std::move(Completion), std::move(Options), false); }

	auto FBuildSession::SubmitImpl(FBuildDefinition Definition, FBuildCompletionCallback Completion, FBuildRequestOptions Options, bool Inline)
		-> std::expected<FBuildRequest, FBuildError>
	{
		auto Error = [](std::string Description) { return FBuildError{.Category = EBuildErrorCategory::Unavailable, .Description = std::move(Description)}; };
		auto Owner = State;
		Private::FSubmissionScope Submission{Owner};
		Private::FExecutionScope Execution(Owner.get());
		std::shared_ptr<Private::FBuildRequestState> Request;
		try
		{
			FBuildDispatcher Dispatch;
			{
				std::lock_guard Lock(Owner->Mutex);
				if (Owner->Closed || !Owner->Resources->Resolver || !Completion || Owner->Active >= 4096)
					return std::unexpected(Error("Build session is closed, full or missing required bindings."));
				if (!Owner->Resources->Registry.Find(Definition.GetFunctionName()))
					return std::unexpected(Error("Build function is not registered."));
				Request = std::make_shared<Private::FBuildRequestState>();
				Request->Id = Owner->NextId++;
				Request->Owner = Owner;
				Request->Data = std::make_shared<Private::FRequestData>(std::move(Definition), std::move(Options), Owner->Resources);
				Request->Callback = std::move(Completion);
				if (!Inline) Dispatch = Owner->Resources->Dispatcher;
				Owner->Requests.emplace(Request->Id, Request); ++Owner->Active;
				++Owner->Submitting; Submission.Counted = true;
			}
			FBuildRequest Handle; Handle.State = Request;
			if (!Dispatch) { Request->Run(); return Handle; }
			auto Ticket = std::make_shared<Private::FDispatchTicket>(); Ticket->Request = Request;
			auto Accepted = Dispatch([Ticket] { Ticket->Request->Run(); });
			Ticket.reset();
			if (!Accepted && Request->RejectDispatch())
			{
				auto Failure = std::move(Accepted.error()); Failure.Phase = EBuildSessionPhase::Dispatch; Failure.BoundDescription();
				return std::unexpected(std::move(Failure));
			}
			Request->AcceptDispatch();
			return Handle;
		}
		catch (...)
		{
			if (Submission.Counted && !Request->RejectDispatch()) { FBuildRequest Handle; Handle.State = Request; return Handle; }
			return std::unexpected(Error("Build dispatch or admission failed."));
		}
	}

	auto FBuildSession::ExecuteInline(FBuildDefinition Definition, FBuildRequestOptions Options) -> FBuildResult
	{
		std::optional<FBuildResult> Result;
		auto Admitted = SubmitImpl(std::move(Definition), [&](FBuildResult Value) { Result = std::move(Value); }, std::move(Options), true);
		if (!Admitted) return std::unexpected(std::move(Admitted.error()));
		return std::move(*Result);
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
