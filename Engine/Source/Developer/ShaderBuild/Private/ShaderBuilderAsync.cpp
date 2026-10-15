#include "ShaderBuilder.h"
#include "ShaderSharedOutput.h"
#include "ShaderCaptureLimits.h"
#include "Shader/ShaderCompiledOutput.h"
#include "Threading/TaskComposition.h"

namespace Durin
{
	namespace
	{
		thread_local uint32 ShaderCallbackDepth = 0;
		auto CanceledShader() -> FShaderBuildResult { return {.Status = EShaderBuildStatus::Canceled}; }
		auto RequestBytes(const FGeneratedShaderCompileRequest& Request) -> uint64
		{
			uint64 Bytes = sizeof(Request) + 3 * Request.Source.capacity() + Request.VirtualPath.capacity() + (Request.ExpectedSourceTree ? Request.ExpectedSourceTree->VirtualPath.capacity() : 0);
			if (Request.ExpectedSourceTree)
			{
				const auto& Tree = *Request.ExpectedSourceTree;
				for (const auto& Entry : Tree.EntryPoints) Bytes += sizeof(Entry) + Entry.capacity();
				Bytes += Tree.Frequencies.capacity() * sizeof(EShaderFrequency);
				for (const auto& Macro : Tree.Macros) Bytes += sizeof(Macro) + Macro.Name.capacity() + (Macro.Value ? Macro.Value->capacity() : 0);
			}
			for (const auto& Entry : Request.EntryPoints) Bytes += sizeof(Entry) + Entry.capacity();
			for (const auto& Macro : Request.Macros) Bytes += sizeof(Macro) + Macro.Name.capacity() + (Macro.Value ? Macro.Value->capacity() : 0);
			for (const auto& Prefix : Request.AllowedImportVirtualPrefixes) Bytes += sizeof(Prefix) + Prefix.capacity();
			if (Request.SourceArtifacts) for (const auto& [Path, Data] : Request.SourceArtifacts->GetFiles()) Bytes += Path.capacity() + 2 * Data.size();
			return Bytes;
		}
	}
	struct FShaderBuilder::FAsyncState : std::enable_shared_from_this<FAsyncState>
	{
		struct FFlight;
		struct FSubscriber final : IShaderBuildRequest
		{
			std::weak_ptr<FAsyncState> State;
			std::shared_ptr<const FGeneratedShaderCompileRequest> Request;
			FShaderBuildRequestOptions Options;
			ESourceKind SourceKind = ESourceKind::Generated;
			FShaderBuildCompletion Completion;
			mutable std::mutex Mutex;
			std::condition_variable Condition;
			FTaskHandle Preparation;
			FTaskCancellationSource PreparationCancellation;
			std::shared_ptr<FFlight> Flight;
			std::atomic_bool Canceled = false;
			enum class EDelivery { Pending, Delivering, Completed };
			EDelivery Delivery = EDelivery::Pending;
			bool Published = false;
			uint64 Bytes = 0;
			auto Finish(FShaderBuildResult Output) -> void
			{
				FShaderBuildCompletion Callback;
				{
					std::lock_guard Lock(Mutex);
					if (Delivery != EDelivery::Pending) return;
					Delivery = EDelivery::Delivering;
					Callback = std::move(Completion);
				}
				try { if (Canceled || (Options.IsCancelled && Options.IsCancelled())) Output = CanceledShader(); } catch (...) { Output = FShaderBuildResult::Failure("Shader cancellation observer failed."); }
				++ShaderCallbackDepth;
				try { if (Callback) Callback(std::move(Output)); } catch (...) { }
				Callback = {};
				--ShaderCallbackDepth;
				{ std::lock_guard Lock(Mutex); Delivery = EDelivery::Completed; }
				Condition.notify_all();
			}
			auto ReleaseCaptures() -> void
			{
				std::shared_ptr<const FGeneratedShaderCompileRequest> RetiredRequest;
				std::function<bool()> RetiredObserver;
				{ std::lock_guard Lock(Mutex); RetiredRequest = std::move(Request); RetiredObserver = std::move(Options.IsCancelled); }
			}
			auto RequestCancel() -> bool override;
			auto SetPriority(EShaderBuildPriority Priority) -> void override;
			auto Wait() -> EShaderBuildWaitResult override;
			auto IsComplete() const -> bool override;
		};
		struct FFlight
		{
			std::string Key;
			std::shared_ptr<FPreparedShaderBuild> Prepared;
			std::unique_ptr<FShaderSessionRequest> Session;
			std::vector<std::shared_ptr<FSubscriber>> Subscribers;
			DerivedData::FRequestOwner Owner;
			uint64 Bytes = 0;
		};
		FShaderBuilder* Builder;
		mutable std::mutex Mutex;
		bool Closed = false;
		uint64 Bytes = 0;
		std::vector<std::shared_ptr<FSubscriber>> Operations;
		std::unordered_map<std::string, std::shared_ptr<FFlight>> Flights;
		std::vector<std::shared_ptr<FFlight>> CompilerOperations;
		explicit FAsyncState(FShaderBuilder* InBuilder) : Builder(InBuilder) {}
		auto PruneCompilerOperations() -> void
		{
			std::erase_if(CompilerOperations, [&](const auto& Flight) {
				if (!Flight->Owner.Poll()) return false;
				Flight->Prepared.reset(); Flight->Session.reset(); Bytes -= Flight->Bytes; return true;
			});
		}
		auto Priority(const std::shared_ptr<FFlight>& Flight) -> void
		{
			auto Priority = EShaderBuildPriority::Low;
			for (const auto& Subscriber : Flight->Subscribers) Priority = std::min(Priority, Subscriber->Options.Priority);
			Flight->Owner.SetPriority(static_cast<DerivedData::ERequestPriority>(Priority));
		}
		auto Complete(const std::shared_ptr<FFlight>& Flight, FShaderBuildResult Output) -> void
		{
			std::vector<std::shared_ptr<FSubscriber>> Subscribers;
			{
				std::lock_guard Lock(Mutex);
				const auto It = Flights.find(Flight->Key);
				if (It != Flights.end() && It->second == Flight) Flights.erase(It);
				Subscribers = std::move(Flight->Subscribers);
			}
			for (const auto& Subscriber : Subscribers) Subscriber->Finish(Output);
		}
		auto Prepare(const std::shared_ptr<FSubscriber>& Subscriber) -> void
		{
			if (Subscriber->Canceled || (Subscriber->Options.IsCancelled && Subscriber->Options.IsCancelled())) return Subscriber->Finish(CanceledShader());
			const auto Reserve = [this, Subscriber](uint64 Amount) {
				std::lock_guard Lock(Mutex);
				if (Amount > 1024ull * 1024 * 1024 - Bytes) return false;
				Bytes += Amount; Subscriber->Bytes += Amount; return true;
			};
			auto Prepared = Subscriber->SourceKind == ESourceKind::Mounted ? Builder->PrepareMounted(Subscriber->Request, Reserve) : Builder->PrepareGenerated(Subscriber->Request, Reserve);
			if (!Prepared) return Subscriber->Finish(FShaderBuildResult::Failure(FormatShaderError(Prepared.error())));
			if (Subscriber->Canceled) return Subscriber->Finish(CanceledShader());
			if (!Subscriber->Request->bForceRecompile)
			{
				auto Retained = Builder->OutputCache.Find((*Prepared)->OutputKey, std::min(ShaderCompiledOutput::MaximumValueBytes, Subscriber->Request->MaximumOutputBytes));
				if (Retained)
				{
					Builder->MemoryHits.fetch_add(1);
					return Subscriber->Finish(std::move(*Retained));
				}
			}
			std::shared_ptr<FFlight> Flight;
			std::unique_ptr<DerivedData::FRequestBarrier> AdmissionBarrier;
			bool Leader = false;
			bool Capacity = false;
			{
				std::lock_guard Lock(Mutex);
				PruneCompilerOperations();
				if (Subscriber->Canceled || Closed) { }
				else
				{
					const auto Key = (*Prepared)->OutputKey + (Subscriber->Request->bForceRecompile ? "/Forced" : "/Cached") + std::format("/Budget{}", Subscriber->Request->MaximumOutputBytes);
					const auto Existing = Flights.find(Key);
					if (Existing != Flights.end()) Flight = Existing->second;
					else if (CompilerOperations.size() < 128)
					{
						Flight = std::make_shared<FFlight>();
						AdmissionBarrier = std::make_unique<DerivedData::FRequestBarrier>(Flight->Owner);
						Flight->Key = Key; Flight->Prepared = std::move(*Prepared);
						Flight->Bytes = std::exchange(Subscriber->Bytes, 0);
						Flights.emplace(Key, Flight); CompilerOperations.push_back(Flight); Leader = true;
					}
					else Capacity = true;
					if (Flight) { Flight->Subscribers.push_back(Subscriber); Subscriber->Flight = Flight; Priority(Flight); }
				}
			}
			if (!Flight) return Subscriber->Finish(Capacity ? FShaderBuildResult::Failure(FormatShaderError({.Code = EShaderError::CaptureInputLimit})) : CanceledShader());
			if (!Leader) return;
			try
			{
				const bool Generated = Flight->Prepared->Recipe.Generated;
				auto Session = MakeShaderSessionRequest(Flight->Prepared->Options, std::move(Flight->Prepared->Recipe), Generated ? std::optional(Flight->Prepared->Request->Source) : std::nullopt);
				if (!Session) return Complete(Flight, FShaderBuildResult::Failure(FormatShaderError(Session.error())));
				Flight->Session = std::make_unique<FShaderSessionRequest>(std::move(*Session));
			}
			catch (...) { return Complete(Flight, FShaderBuildResult::Failure(FormatShaderError({.Code = EShaderError::InvalidCompileRequest}))); }
			SubmitFlight(Flight, Subscriber->Options.bInlineExecution, true);
		}
		auto SubmitFlight(const std::shared_ptr<FFlight>& Flight, bool Inline, bool Query) -> void
		{
			DerivedData::FRequestBarrier Submission(Flight->Owner);
			try {
			using namespace DerivedData;
			FBuildRequestOptions Options;
			Options.Policy.ForceBuild = Flight->Prepared->Options.bForceRecompile;
			Options.Policy.QueryCache = Query;
			Options.Policy.InputLimits.MaximumTotalBytes = ShaderCaptureLimits::MaximumSessionInputBytes;
			Options.Policy.InputLimits.MaximumValues = uint32(ShaderCaptureLimits::MaximumFiles + 2);
			Options.Policy.OutputLimits.MaximumTotalBytes = std::min(ShaderCompiledOutput::MaximumValueBytes, Flight->Prepared->Request->MaximumOutputBytes);
			Options.Policy.PersistenceLimits = Options.Policy.OutputLimits;
			Options.Policy.MaximumEncodedBytes = ShaderCompiledOutput::MaximumValueBytes + 4ull * 1024 * 1024;
			auto Admitted = Builder->BuildService->Submit(*Flight->Session, Flight->Owner,
				[Self = shared_from_this(), Flight, Inline, Query](FBuildCompleteParams Completion) {
					try
					{
					FShaderBuildResult Output;
					if (HasBuildExecutionFlag(Completion.GetExecutionFlags(), EBuildExecutionFlags::BuildLocal)) Self->Builder->Compilations.fetch_add(1);
					if (HasBuildExecutionFlag(Completion.GetExecutionFlags(), EBuildExecutionFlags::CacheQueryHit)) Self->Builder->DdcHits.fetch_add(1);
					if (Completion.GetStatus() == EStatus::Canceled) Output = CanceledShader();
					else if (Completion.GetStatus() == EStatus::Ok && Completion.GetOutput())
					{
						auto Built = ShaderSharedOutput::Assemble(Flight->Prepared->Options, *Completion.GetOutput());
						if (!Built && Query && HasBuildExecutionFlag(Completion.GetExecutionFlags(), EBuildExecutionFlags::CacheQueryHit) && !Flight->Owner.IsCanceled()) { Self->SubmitFlight(Flight, Inline, false); return; }
						Output = Built ? FShaderBuildResult::Success(std::move(*Built)) : FShaderBuildResult::Failure(FormatShaderError(Built.error()));
						if (Output && !Flight->Owner.IsCanceled()) Self->Builder->OutputCache.Store(Flight->Prepared->OutputKey, Output, *Completion.GetOutput());
					}
					else
					{
						std::string Diagnostic(Completion.GetDiagnostic());
						if (const auto* Product = Completion.GetOutput())
							for (const auto& Message : Product->GetMessages())
							{
								if (!Diagnostic.empty()) Diagnostic += '\n';
								Diagnostic.append(Message.Text, 0, 4096 - std::min<size_t>(Diagnostic.size(), 4096));
								if (Diagnostic.size() >= 4096) break;
							}
						Output = FShaderBuildResult::Failure(std::move(Diagnostic));
					}
					Self->Complete(Flight, std::move(Output));
					}
					catch (...) { Self->Complete(Flight, FShaderBuildResult::Failure(FormatShaderError({.Code = EShaderError::InvalidCompileRequest}))); }
				}, std::move(Options), Inline);
			if (!Admitted) Complete(Flight, FShaderBuildResult::Failure(Admitted.error().Description));
			} catch (...) { Complete(Flight, FShaderBuildResult::Failure(FormatShaderError({.Code = EShaderError::InvalidCompileRequest}))); }
		}
	};
	auto FShaderBuilder::FAsyncState::FSubscriber::RequestCancel() -> bool
	{
		if (Canceled.exchange(true)) return false;
		PreparationCancellation.RequestCancellation();
		std::shared_ptr<FFlight> CancelFlight;
		if (auto Owner = State.lock())
		{
			std::lock_guard Lock(Owner->Mutex);
			if (Flight)
			{
				std::erase_if(Flight->Subscribers, [&](const auto& Item) { return Item.get() == this; });
				Owner->Priority(Flight);
				if (Flight->Subscribers.empty())
				{
					CancelFlight = Flight;
					const auto It = Owner->Flights.find(Flight->Key);
					if (It != Owner->Flights.end() && It->second == Flight) Owner->Flights.erase(It);
				}
				else Flight.reset();
			}
		}
		if (CancelFlight) CancelFlight->Owner.RequestCancel();
		bool PublishedNow;
		{ std::lock_guard Lock(Mutex); PublishedNow = Published; }
		if (PublishedNow) Finish(CanceledShader());
		return true;
	}
	auto FShaderBuilder::FAsyncState::FSubscriber::SetPriority(EShaderBuildPriority Priority) -> void
	{
		if (auto Owner = State.lock()) { std::lock_guard Lock(Owner->Mutex); Options.Priority = Priority; if (Flight) Owner->Priority(Flight); }
	}
	auto FShaderBuilder::FAsyncState::FSubscriber::IsComplete() const -> bool
	{
		{ std::lock_guard Lock(Mutex); if (Delivery != EDelivery::Completed || !Published || (Preparation.IsValid() && !Preparation.IsComplete())) return false; }
		if (auto Owner = State.lock()) { std::lock_guard Lock(Owner->Mutex); return !Flight || Flight->Owner.Poll(); }
		return true;
	}
	auto FShaderBuilder::FAsyncState::FSubscriber::Wait() -> EShaderBuildWaitResult
	{
		if (ShaderCallbackDepth) return EShaderBuildWaitResult::WouldBlock;
		FTaskHandle Task;
		{ std::unique_lock Lock(Mutex); Condition.wait(Lock, [&] { return Published; }); Task = Preparation; }
		if (Task.IsValid() && WaitTask(Task).WaitStatus != ETaskWaitStatus::Completed) return EShaderBuildWaitResult::WouldBlock;
		std::shared_ptr<FFlight> Pending;
		if (auto Owner = State.lock()) { std::lock_guard Lock(Owner->Mutex); Pending = Flight; }
		if (Pending && Pending->Owner.Wait() != DerivedData::ERequestWaitResult::Completed) return EShaderBuildWaitResult::WouldBlock;
		{ std::unique_lock Lock(Mutex); Condition.wait(Lock, [&] { return Delivery == EDelivery::Completed; }); }
		ReleaseCaptures();
		if (auto Owner = State.lock()) { std::lock_guard Lock(Owner->Mutex); Owner->PruneCompilerOperations(); }
		return EShaderBuildWaitResult::Completed;
	}
	auto FShaderBuilder::SubmitRequest(FGeneratedShaderCompileRequest Request, FShaderBuildRequestOptions Options,
		FShaderBuildCompletion Completion, ESourceKind SourceKind) -> std::expected<FShaderBuildHandle, FShaderBuildAdmissionError>
	{
		if (!Completion) return std::unexpected(FShaderBuildAdmissionError{});
		if (!Options.bInlineExecution && !IsTaskSchedulerRunning()) return std::unexpected(FShaderBuildAdmissionError{EShaderBuildAdmissionReason::DispatchRejected});
		auto Subscriber = std::make_shared<FAsyncState::FSubscriber>();
		Subscriber->SourceKind = SourceKind; Subscriber->State = Async; Subscriber->Bytes = RequestBytes(Request);
		Subscriber->Request = std::make_shared<const FGeneratedShaderCompileRequest>(std::move(Request));
		Subscriber->Options = std::move(Options); Subscriber->Completion = std::move(Completion);
		// Snapshot retired controls outside the state lock: retirement queries also
		// acquire that lock and must never call back while admission holds it.
		std::vector<std::shared_ptr<FAsyncState::FSubscriber>> Retired;
		{ std::lock_guard Lock(Async->Mutex); Retired = Async->Operations; }
		std::erase_if(Retired, [](const auto& Item) { return !Item->IsComplete(); });
		for (const auto& Item : Retired) Item->ReleaseCaptures();
		{
			std::lock_guard Lock(Async->Mutex);
			for (const auto& Item : Retired)
			{
				const auto It = std::ranges::find(Async->Operations, Item);
				if (It != Async->Operations.end()) { Async->Bytes -= Item->Bytes; Async->Operations.erase(It); }
			}
			Async->PruneCompilerOperations();
			if (Async->Closed) return std::unexpected(FShaderBuildAdmissionError{EShaderBuildAdmissionReason::Closed});
			if (Async->Operations.size() >= 512 || Subscriber->Bytes > 1024ull * 1024 * 1024 - Async->Bytes)
				return std::unexpected(FShaderBuildAdmissionError{EShaderBuildAdmissionReason::Capacity});
			Async->Bytes += Subscriber->Bytes; Async->Operations.push_back(Subscriber);
		}
		if (Subscriber->Options.bInlineExecution)
		{
			try { Async->Prepare(Subscriber); } catch (...) { Subscriber->Finish(FShaderBuildResult::Failure(FormatShaderError({.Code = EShaderError::InvalidCompileRequest}))); }
		}
		else
		{
			auto Dispatch = Private::TryLaunchCancelableTaskWithCompletion("ShaderPrepare",
				[Self = Async, Subscriber](const FTaskCancellationToken&) { try { Self->Prepare(Subscriber); } catch (...) { Subscriber->Finish(FShaderBuildResult::Failure(FormatShaderError({.Code = EShaderError::InvalidCompileRequest}))); } },
				[Subscriber](ETaskState State) { if (State == ETaskState::Canceled) Subscriber->Finish(CanceledShader()); else if (State != ETaskState::Succeeded) Subscriber->Finish(FShaderBuildResult::Failure("Shader preparation task failed.")); }, {.CancellationToken = Subscriber->PreparationCancellation.GetToken(), .Priority = static_cast<ETaskPriority>(Subscriber->Options.Priority)});
			if (!Dispatch.HasValue())
			{
				{ std::lock_guard Lock(Async->Mutex); Async->Bytes -= Subscriber->Bytes; std::erase(Async->Operations, Subscriber); }
				{ std::lock_guard Lock(Subscriber->Mutex); Subscriber->Completion = {}; Subscriber->Delivery = FAsyncState::FSubscriber::EDelivery::Completed; Subscriber->Published = true; }
				Subscriber->Condition.notify_all();
				return std::unexpected(FShaderBuildAdmissionError{EShaderBuildAdmissionReason::DispatchRejected});
			}
			Subscriber->Preparation = std::move(Dispatch).TakeValue();
		}
		{ std::lock_guard Lock(Subscriber->Mutex); Subscriber->Published = true; }
		Subscriber->Condition.notify_all();
		return FShaderBuildHandle(Subscriber);
	}
	auto FShaderBuilder::SubmitGenerated(FGeneratedShaderCompileRequest Request, FShaderBuildRequestOptions Options,
		FShaderBuildCompletion Completion) -> std::expected<FShaderBuildHandle, FShaderBuildAdmissionError>
	{ return SubmitRequest(std::move(Request), std::move(Options), std::move(Completion), ESourceKind::Generated); }
	auto FShaderBuilder::GetOrCompileGenerated(const FGeneratedShaderCompileRequest& Request) -> FShaderBuildResult
	{
		FShaderBuildResult Output;
		auto Submitted = SubmitGenerated(Request, {.bInlineExecution = true}, [&](auto Result) { Output = std::move(Result); });
		if (!Submitted) return FShaderBuildResult::Failure(FormatShaderError({.Code = EShaderError::InvalidCompileRequest}));
		require(Submitted->Wait() == EShaderBuildWaitResult::Completed);
		return Output;
	}
	auto FShaderBuilder::Close() -> void
	{
		if (!Async) return;
		std::vector<std::shared_ptr<FAsyncState::FSubscriber>> Pending;
		{ std::lock_guard Lock(Async->Mutex); Async->Closed = true; Pending = Async->Operations; }
		for (const auto& Subscriber : Pending) Subscriber->RequestCancel();
		for (const auto& Subscriber : Pending) require(Subscriber->Wait() == EShaderBuildWaitResult::Completed);
		{ std::lock_guard Lock(Async->Mutex); Async->Operations.clear(); Async->Flights.clear(); Async->CompilerOperations.clear(); Async->Bytes = 0; }
	}
	auto FShaderBuilder::InitializeAsync() -> void { Async = std::make_shared<FAsyncState>(this); }
}
