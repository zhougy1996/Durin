#include "ShaderBuilder.h"
#include "MetalLibraryBuild.h"
#include "MetalShaderFormat.h"
#include "ShaderSharedOutput.h"
#include "ShaderCaptureLimits.h"
#include "Shader/ShaderCompiledOutput.h"
#include "Threading/TaskComposition.h"

namespace Durin
{
	namespace
	{
		thread_local uint32 ShaderCallbackDepth = 0;
		auto CanceledShader() -> FShaderCompilerOutput { return {.Status = EShaderBuildStatus::Canceled}; }
		auto NativeResultBytes(const FShaderCompilerOutput& Result) -> uint64
		{
			uint64 Bytes = sizeof(Result) + Result.CompiledShaders.capacity() * sizeof(FShaderCode);
			Bytes += Result.Errors.capacity() * sizeof(FShaderCompilerError);
			for (const auto& Error : Result.Errors) Bytes += Error.ErrorVirtualFilePath.capacity() + Error.ErrorLineString.capacity()
				+ Error.StrippedErrorMessage.capacity() + Error.HighlightedLine.capacity() + Error.HighlightedLineMarker.capacity();
			for (const auto& Shader : Result.CompiledShaders)
			{
				Bytes += Shader.Code.GetRetainedCapacityBytes() + Shader.PlatformData.GetRetainedCapacityBytes()
					+ Shader.Reflection.ResourceBindings.capacity() * sizeof(FShaderResourceBinding)
					+ Shader.Reflection.PushConstantRanges.capacity() * sizeof(FPushConstantRange)
					+ Shader.SourceEntryPoint.capacity() + Shader.BinaryEntryPoint.capacity() + Shader.DebugName.capacity();
				for (const auto& Binding : Shader.Reflection.ResourceBindings) Bytes += Binding.Name.capacity();
			}
			return Bytes;
		}

	}
	struct FShaderBuilder::FAsyncState : std::enable_shared_from_this<FAsyncState>
	{
		struct FFlight;
		struct FSubscriber final : IShaderBuildRequest, std::enable_shared_from_this<FSubscriber>
		{
			std::weak_ptr<FAsyncState> State;
			std::shared_ptr<const FGeneratedShaderCompileRequest> Request;
			FShaderBuildRequestOptions Options;
			ESourceKind SourceKind = ESourceKind::Generated;
			FShaderBuildCompletion Completion;
			mutable std::mutex Mutex;
			std::condition_variable Condition;
			Tasks::FTask Preparation;
			FTaskCancellationSource PreparationCancellation;
			std::shared_ptr<FFlight> Flight;
			std::atomic_bool Canceled = false;
			enum class EDelivery { Pending, Delivering, Completed };
			EDelivery Delivery = EDelivery::Pending;
			bool Published = false;
			bool BinaryMetal = false;
			std::atomic_bool NativeStarted = false;
			std::string NativeProducer;
			DerivedData::FRequestOwner NativeOwner;
			FShaderCompilerOutput NativeOutput;
			size_t NativeIndex = 0;
			auto Finish(FShaderCompilerOutput Output) -> void
			{
				if (BinaryMetal && Output && !NativeStarted.exchange(true))
				{
					if (NativeResultBytes(Output) > Request->MaximumOutputBytes)
						Output = FShaderCompilerOutput::Failure("Metal source result exceeds retained-output budget.");
					else if (auto Owner = State.lock())
					{
						NativeOutput = std::move(Output);
						return Owner->SubmitNative(shared_from_this());
					}
					else Output = FShaderCompilerOutput::Failure("Shader build owner was retired.");
				}
				FShaderBuildCompletion Callback;
				{
					std::lock_guard Lock(Mutex);
					if (Delivery != EDelivery::Pending) return;
					Delivery = EDelivery::Delivering;
					Callback = std::move(Completion);
				}
				try { if (Canceled || (Options.IsCancelled && Options.IsCancelled())) Output = CanceledShader(); } catch (...) { Output = FShaderCompilerOutput::Failure("Shader cancellation observer failed."); }
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
				{ std::lock_guard Lock(Mutex); RetiredRequest = std::move(Request); RetiredObserver = std::move(Options.IsCancelled); NativeOutput = {}; }
			}
			auto Start() -> void;
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
		};
		FShaderBuilder* Builder;
		mutable std::mutex Mutex;
		bool Closed = false;
		std::vector<std::shared_ptr<FSubscriber>> Operations;
		std::unordered_map<std::string, std::shared_ptr<FFlight>> Flights;
		std::vector<std::shared_ptr<FFlight>> CompilerOperations;
		explicit FAsyncState(FShaderBuilder* InBuilder) : Builder(InBuilder) {}
		auto PruneCompilerOperations() -> void
		{
			std::erase_if(CompilerOperations, [&](const auto& Flight) {
				if (!Flight->Owner.Poll()) return false;
				Flight->Prepared.reset(); Flight->Session.reset(); return true;
			});
		}
		auto Priority(const std::shared_ptr<FFlight>& Flight) -> void
		{
			auto Priority = EShaderBuildPriority::Low;
			for (const auto& Subscriber : Flight->Subscribers) Priority = std::min(Priority, Subscriber->Options.Priority);
			Flight->Owner.SetPriority(static_cast<DerivedData::ERequestPriority>(Priority));
		}
		auto Complete(const std::shared_ptr<FFlight>& Flight, FShaderCompilerOutput Output) -> void
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
			if (Subscriber->BinaryMetal && !Subscriber->Options.NativeProducerIdentity.empty())
				Subscriber->NativeProducer = Subscriber->Options.NativeProducerIdentity;
			else if (Subscriber->BinaryMetal)
			{
				const auto Producer = GetMetalLibraryProducerIdentity(DerivedData::FBuildCancellation([Weak = std::weak_ptr(Subscriber)] {
					const auto Owner = Weak.lock(); return !Owner || Owner->Canceled.load()
					|| (Owner->Options.IsCancelled && Owner->Options.IsCancelled());
				}));
				if (!Producer) return Subscriber->Finish(FShaderCompilerOutput::Failure(Producer.error()));
				Subscriber->NativeProducer = *Producer;
			}
			auto Prepared = Subscriber->SourceKind == ESourceKind::Mounted ? Builder->PrepareMounted(Subscriber->Request) : Builder->PrepareGenerated(Subscriber->Request);
			if (!Prepared) return Subscriber->Finish(FShaderCompilerOutput::Failure(Prepared.error()));
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
			{
				std::lock_guard Lock(Mutex);
				PruneCompilerOperations();
				if (Subscriber->Canceled || Closed) { }
				else
				{
					const auto Key = (*Prepared)->OutputKey + (Subscriber->Request->bForceRecompile ? "/Forced" : "/Cached") + std::format("/Budget{}", Subscriber->Request->MaximumOutputBytes);
					const auto Existing = Flights.find(Key);
					if (Existing != Flights.end()) Flight = Existing->second;
					else
					{
						Flight = std::make_shared<FFlight>();
						AdmissionBarrier = std::make_unique<DerivedData::FRequestBarrier>(Flight->Owner);
						Flight->Key = Key; Flight->Prepared = std::move(*Prepared);

						Flights.emplace(Key, Flight); CompilerOperations.push_back(Flight); Leader = true;
					}
					if (Flight) { Flight->Subscribers.push_back(Subscriber); Subscriber->Flight = Flight; Priority(Flight); }
				}
			}
			if (!Flight) return Subscriber->Finish(CanceledShader());
			if (!Leader) return;
			try
			{
				const bool Generated = Flight->Prepared->Recipe.Generated;
				auto Session = MakeShaderSessionRequest(Flight->Prepared->Options, std::move(Flight->Prepared->Recipe), Generated ? std::optional(Flight->Prepared->Request->Source) : std::nullopt);
				if (!Session) return Complete(Flight, FShaderCompilerOutput::Failure(Session.error()));
				Flight->Session = std::make_unique<FShaderSessionRequest>(std::move(*Session));
			}
			catch (...) { return Complete(Flight, FShaderCompilerOutput::Failure(std::string("Shader entry-point request is invalid."))); }
			SubmitFlight(Flight, Subscriber->Options.bInlineExecution, true);
		}
		auto SubmitNative(const std::shared_ptr<FSubscriber>& Subscriber, bool Query = true) -> void
		{
			using namespace DerivedData;
			FRequestBarrier Barrier(Subscriber->NativeOwner);
			if (Subscriber->Canceled) return Subscriber->Finish(CanceledShader());
			if (Subscriber->NativeIndex == Subscriber->NativeOutput.CompiledShaders.size())
				return Subscriber->Finish(std::move(Subscriber->NativeOutput));
			auto Request = MakeMetalLibraryBuildRequest(Subscriber->NativeOutput.CompiledShaders[Subscriber->NativeIndex], Subscriber->NativeProducer);
			if (!Request) return Subscriber->Finish(FShaderCompilerOutput::Failure(Request.error()));
			FBuildRequestOptions Options;
			Options.Policy.ForceBuild = Subscriber->Request->bForceRecompile;
			Options.Policy.QueryCache = Query;
			Options.Policy.BuildLocal = !Subscriber->Options.bNativeCacheOnly;
			Options.Policy.OutputLimits.MaximumTotalBytes = Subscriber->Request->MaximumOutputBytes;
			Options.Policy.PersistenceLimits = Options.Policy.OutputLimits;
			Options.Cancellation = FBuildCancellation([Weak = std::weak_ptr(Subscriber)] {
				const auto Owner = Weak.lock(); return !Owner || Owner->Canceled.load()
					|| (Owner->Options.IsCancelled && Owner->Options.IsCancelled());
			});
			auto Admitted = Builder->BuildService->Submit(std::move(*Request), Subscriber->NativeOwner,
				[Self = shared_from_this(), Subscriber, Query](FBuildCompleteParams Completion) {
					try
					{
						if (Completion.GetStatus() == EStatus::Canceled) return Subscriber->Finish(CanceledShader());
						if (Completion.GetStatus() != EStatus::Ok || !Completion.GetOutput())
							return Subscriber->Finish(FShaderCompilerOutput::Failure(std::string(Completion.GetDiagnostic())));
						const auto* Value = Completion.GetOutput()->FindValue(FValueId::FromName("Library"));
						if (!Value || !ShaderFormat::ValidateMetalLibrary(Value->GetData().GetBytes()))
						{
							if (Query && HasBuildExecutionFlag(Completion.GetExecutionFlags(), EBuildExecutionFlags::CacheQueryHit)) return Self->SubmitNative(Subscriber, false);
							return Subscriber->Finish(FShaderCompilerOutput::Failure("Invalid cached Metal library."));
						}
						auto& Shader = Subscriber->NativeOutput.CompiledShaders[Subscriber->NativeIndex];
						Shader.Code = Value->GetData();
						Shader.CodeHash = FXxHash128::HashBuffer(Shader.Code);
						Shader.Target = MetalLibraryShaderTarget;
						Shader.CodeFormat = EShaderCodeFormat::MetalLibrary;
						Shader.ShaderIdentity = ShaderFormat::ComputeShaderIdentity(Shader);
						if (NativeResultBytes(Subscriber->NativeOutput) > Subscriber->Request->MaximumOutputBytes)
							return Subscriber->Finish(FShaderCompilerOutput::Failure("Metal stage set exceeds output budget."));
						++Subscriber->NativeIndex;
						Self->SubmitNative(Subscriber);
					}
					catch (...) { Subscriber->Finish(FShaderCompilerOutput::Failure("Metal library completion failed.")); }
				}, Options, Subscriber->Options.bInlineExecution);
			if (!Admitted) Subscriber->Finish(FShaderCompilerOutput::Failure(Admitted.error().Description));
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
					FShaderCompilerOutput Output;
					if (HasBuildExecutionFlag(Completion.GetExecutionFlags(), EBuildExecutionFlags::BuildLocal)) Self->Builder->Compilations.fetch_add(1);
					if (HasBuildExecutionFlag(Completion.GetExecutionFlags(), EBuildExecutionFlags::CacheQueryHit)) Self->Builder->DdcHits.fetch_add(1);
					if (Completion.GetStatus() == EStatus::Canceled) Output = CanceledShader();
					else if (Completion.GetStatus() == EStatus::Ok && Completion.GetOutput())
					{
						auto Built = ShaderSharedOutput::Assemble(Flight->Prepared->Options, *Completion.GetOutput());
						if (!Built && Query && HasBuildExecutionFlag(Completion.GetExecutionFlags(), EBuildExecutionFlags::CacheQueryHit) && !Flight->Owner.IsCanceled()) { Self->SubmitFlight(Flight, Inline, false); return; }
						Output = std::move(Built);
						if (Output && !Flight->Owner.IsCanceled()) Self->Builder->OutputCache.Store(Flight->Prepared->OutputKey, Output, *Completion.GetOutput());
					}
					else
					{
						std::string Diagnostic(Completion.GetDiagnostic());
						if (const auto* Product = Completion.GetOutput())
							for (const auto& Message : Product->GetMessages())
							{
								if (!Diagnostic.empty()) Diagnostic += '\n';
								Diagnostic.append(Message.Text, 0, ShaderBuildDiagnosticMaxBytes - std::min(Diagnostic.size(), ShaderBuildDiagnosticMaxBytes));
								if (Diagnostic.size() >= ShaderBuildDiagnosticMaxBytes) break;
							}
						if (const auto* Product = Completion.GetOutput()) Output.Errors = ShaderSharedOutput::ReadErrors(*Product);
						if (Output.Errors.empty()) Output.AddError(std::move(Diagnostic));
					}
					Self->Complete(Flight, std::move(Output));
					}
					catch (...) { Self->Complete(Flight, FShaderCompilerOutput::Failure(std::string("Shader entry-point request is invalid."))); }
				}, std::move(Options), Inline);
			if (!Admitted) Complete(Flight, FShaderCompilerOutput::Failure(Admitted.error().Description));
			} catch (...) { Complete(Flight, FShaderCompilerOutput::Failure(std::string("Shader entry-point request is invalid."))); }
		}
	};
	auto FShaderBuilder::FAsyncState::FSubscriber::RequestCancel() -> bool
	{
		if (Canceled.exchange(true)) return false;
		NativeOwner.RequestCancel();
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
		NativeOwner.SetPriority(static_cast<DerivedData::ERequestPriority>(Priority));
		if (auto Owner = State.lock()) { std::lock_guard Lock(Owner->Mutex); Options.Priority = Priority; if (Flight) Owner->Priority(Flight); }
	}
	auto FShaderBuilder::FAsyncState::FSubscriber::IsComplete() const -> bool
	{
		{ std::lock_guard Lock(Mutex); if (Delivery != EDelivery::Completed || !Published || (Preparation.IsValid() && !Preparation.IsCompleted())) return false; }
		if (!NativeOwner.Poll()) return false;
		if (auto Owner = State.lock()) { std::lock_guard Lock(Owner->Mutex); return !Flight || Flight->Owner.Poll(); }
		return true;
	}
	auto FShaderBuilder::FAsyncState::FSubscriber::Wait() -> EShaderBuildWaitResult
	{
		if (ShaderCallbackDepth) return EShaderBuildWaitResult::WouldBlock;
		Tasks::FTask Task;
		{ std::unique_lock Lock(Mutex); Condition.wait(Lock, [&] { return Published; }); Task = Preparation; }
		if (Task.IsValid() && WaitTask(Task).WaitStatus != ETaskWaitStatus::Completed) return EShaderBuildWaitResult::WouldBlock;
		std::shared_ptr<FFlight> Pending;
		if (auto Owner = State.lock()) { std::lock_guard Lock(Owner->Mutex); Pending = Flight; }
		if (Pending && Pending->Owner.Wait() != DerivedData::ERequestWaitResult::Completed) return EShaderBuildWaitResult::WouldBlock;
		if (NativeOwner.Wait() != DerivedData::ERequestWaitResult::Completed) return EShaderBuildWaitResult::WouldBlock;
		{ std::unique_lock Lock(Mutex); Condition.wait(Lock, [&] { return Delivery == EDelivery::Completed; }); }
		ReleaseCaptures();
		if (auto Owner = State.lock()) { std::lock_guard Lock(Owner->Mutex); Owner->PruneCompilerOperations(); }
		return EShaderBuildWaitResult::Completed;
	}
	auto FShaderBuilder::CompileRequest(FGeneratedShaderCompileRequest Request, FShaderBuildRequestOptions Options,
		FShaderBuildCompletion Completion, ESourceKind SourceKind) -> FShaderBuildHandle
	{
		require(Completion);
		auto Subscriber = std::make_shared<FAsyncState::FSubscriber>();
		Subscriber->BinaryMetal = Request.Target == MetalLibraryShaderTarget;
		if (Subscriber->BinaryMetal) Request.Target = MetalShaderTarget;
		Subscriber->SourceKind = SourceKind; Subscriber->State = Async;
		Subscriber->Request = std::make_shared<const FGeneratedShaderCompileRequest>(std::move(Request));
		Subscriber->Options = std::move(Options); Subscriber->Completion = std::move(Completion);
		Subscriber->Start();
		return FShaderBuildHandle(std::move(Subscriber));
	}
	auto FShaderBuilder::FAsyncState::FSubscriber::Start() -> void
	{
		const auto Subscriber = shared_from_this();
		const auto Owner = State.lock();
		const auto Publish = [this] {
			{ std::lock_guard Lock(Mutex); Published = true; }
			Condition.notify_all();
		};
		const auto Complete = [this, &Publish](FShaderCompilerOutput Output) { Finish(std::move(Output)); Publish(); };
		if (!Owner) { Canceled = true; return Complete(CanceledShader()); }
		{
			std::lock_guard Lock(Owner->Mutex);
			if (Owner->Closed) Canceled = true;
		}
		if (Canceled) return Complete(CanceledShader());
		if ((!BinaryMetal && (!Options.NativeProducerIdentity.empty() || Options.bNativeCacheOnly))
			|| Options.NativeProducerIdentity.size() > 512
			|| std::ranges::any_of(Options.NativeProducerIdentity, [](unsigned char Value) { return Value < 32 || Value > 126; })
			|| (Options.bNativeCacheOnly && (Options.NativeProducerIdentity.empty() || Request->bForceRecompile)))
			return Complete(FShaderCompilerOutput::Failure("Shader native producer options are invalid."));
		if (BinaryMetal && Request->MaximumOutputBytes > ShaderCompiledOutput::MaximumValueBytes)
			return Complete(FShaderCompilerOutput::Failure("Shader output exceeds the supported byte limit."));
		if (!Options.bInlineExecution && !IsTaskSchedulerRunning())
			return Complete(FShaderCompilerOutput::Failure("Shader preparation task scheduler is unavailable."));
		std::vector<std::shared_ptr<FSubscriber>> Retired;
		{ std::lock_guard Lock(Owner->Mutex); Retired = Owner->Operations; }
		std::erase_if(Retired, [](const auto& Item) { return !Item->IsComplete(); });
		for (const auto& Item : Retired) Item->ReleaseCaptures();
		{
			std::lock_guard Lock(Owner->Mutex);
			for (const auto& Item : Retired) {
				const auto It = std::ranges::find(Owner->Operations, Item);
				if (It != Owner->Operations.end()) { Owner->Operations.erase(It); }
			}
			Owner->PruneCompilerOperations();
			if (Owner->Closed) Canceled = true;
			else { Owner->Operations.push_back(Subscriber); }
		}
		if (Canceled) return Complete(CanceledShader());
		if (Options.bInlineExecution) {
			try { Owner->Prepare(Subscriber); }
			catch (...) { Finish(FShaderCompilerOutput::Failure("Shader entry-point request is invalid.")); }
		}
		else {
			EShaderBuildPriority Priority;
			{ std::lock_guard Lock(Owner->Mutex); Priority = Options.Priority; }
			auto Dispatch = Private::TryLaunchCancelableTaskWithCompletion("ShaderPrepare",
				[Owner, Subscriber](const FTaskCancellationToken& Token) {
					try { if (Token.IsCancellationRequested()) Subscriber->Finish(CanceledShader()); else Owner->Prepare(Subscriber); }
					catch (...) { Subscriber->Finish(FShaderCompilerOutput::Failure("Shader entry-point request is invalid.")); }
				}, [Subscriber](ETaskState State) {
					if (State != ETaskState::Succeeded) Subscriber->Finish(FShaderCompilerOutput::Failure("Shader preparation task failed."));
				}, {.CancellationToken = PreparationCancellation.GetToken(), .Priority = static_cast<ETaskPriority>(Priority)});
			if (!Dispatch.HasValue()) Finish(FShaderCompilerOutput::Failure("Shader preparation task dispatch failed."));
			else { std::lock_guard Lock(Mutex); Preparation = std::move(Dispatch).TakeValue(); }
		}
		Publish();
	}
	auto FShaderBuilder::CompileGenerated(FGeneratedShaderCompileRequest Request, FShaderBuildRequestOptions Options,
		FShaderBuildCompletion Completion) -> FShaderBuildHandle
	{ return CompileRequest(std::move(Request), std::move(Options), std::move(Completion), ESourceKind::Generated); }
	auto FShaderBuilder::GetOrCompileGenerated(const FGeneratedShaderCompileRequest& Request) -> FShaderCompilerOutput
	{
		FShaderCompilerOutput Output;
		auto Job = CompileGenerated(Request, {.bInlineExecution = true}, [&](auto Result) { Output = std::move(Result); });
		require(Job.Wait() == EShaderBuildWaitResult::Completed);
		return Output;
	}
	auto FShaderBuilder::Close() -> void
	{
		if (!Async) return;
		std::vector<std::shared_ptr<FAsyncState::FSubscriber>> Pending;
		{ std::lock_guard Lock(Async->Mutex); Async->Closed = true; Pending = Async->Operations; }
		for (const auto& Subscriber : Pending) Subscriber->RequestCancel();
		for (const auto& Subscriber : Pending) require(Subscriber->Wait() == EShaderBuildWaitResult::Completed);
		{ std::lock_guard Lock(Async->Mutex); Async->Operations.clear(); Async->Flights.clear(); Async->CompilerOperations.clear(); }
	}
	auto FShaderBuilder::InitializeAsync() -> void { Async = std::make_shared<FAsyncState>(this); }
}
