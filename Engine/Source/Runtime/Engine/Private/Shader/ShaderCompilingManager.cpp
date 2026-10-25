#include "Shader/ShaderCompilingManager.h"
#include "Shader/IShaderBuildModule.h"
#include "Threading/RunnableThread.h"
#include "Materials/MaterialPreparedProgram.h"
#include "Materials/MaterialCompileLifecycle.h"
#if DURIN_WITH_EDITOR
#include "Materials/MaterialTranslation.h"
#endif

namespace Durin
{
	namespace
	{
		thread_local uint32 ShaderGroupCallbackDepth = 0;
		auto RaisePeak(std::atomic_uint64_t& Peak, uint64 Value) -> void
		{ auto Previous = Peak.load(); while (Previous < Value && !Peak.compare_exchange_weak(Previous, Value)) {} }
		auto RetainedBytes(const FShaderCompilerOutput& Output) -> uint64
		{
			uint64 Bytes = sizeof(Output) + Output.CompiledShaders.capacity() * sizeof(FShaderCode);
			Bytes += Output.Errors.capacity() * sizeof(FShaderCompilerError);
			for (const auto& Error : Output.Errors) Bytes += Error.ErrorVirtualFilePath.capacity() + Error.ErrorLineString.capacity()
				+ Error.StrippedErrorMessage.capacity() + Error.HighlightedLine.capacity() + Error.HighlightedLineMarker.capacity();
			for (const auto& Shader : Output.CompiledShaders) {
				Bytes += Shader.Code.GetRetainedCapacityBytes() + Shader.PlatformData.GetRetainedCapacityBytes()
					+ Shader.SourceEntryPoint.capacity() + Shader.BinaryEntryPoint.capacity() + Shader.DebugName.capacity()
					+ Shader.Reflection.ResourceBindings.capacity() * sizeof(FShaderResourceBinding)
					+ Shader.Reflection.PushConstantRanges.capacity() * sizeof(FPushConstantRange);
				for (const auto& Binding : Shader.Reflection.ResourceBindings) Bytes += Binding.Name.capacity();
			}
			return Bytes;
		}
	}
	// One common dispatch window and ordered result set for a ShaderMap or global request.
	struct FShaderCompilingManager::FJobGroup final : IShaderBuildRequest, std::enable_shared_from_this<FJobGroup>
	{
		FShaderCompilingManager* Manager = nullptr;
		IShaderBuildModule* Backend = nullptr;
		mutable std::mutex Mutex;
		std::condition_variable Condition;
		std::vector<FGeneratedShaderCompileRequest> Jobs;
		std::vector<std::optional<FShaderCompilerOutput>> Outputs;
		std::vector<std::shared_ptr<FShaderBuildHandle>> Handles;
		std::string MountedPath;
		FShaderCompileOptions MountedOptions;
		FShaderBuildRequestOptions Options;
		FShaderBuildCompletion Completion;
		uint64 MaximumBytes = 0, Retained = 0, Reserved = 0;
		uint64 MaterialCompilingId = 0;
		size_t Next = 0, Active = 0;
		bool Dispatching = false, Delivering = false, Completed = false, Published = false;
		std::atomic_bool Canceled = false, Submitted = false;
		auto RequestCancel() -> bool override
		{
			const bool Changed = !Canceled.exchange(true);
			std::vector<std::shared_ptr<FShaderBuildHandle>> Pending;
			{ std::lock_guard Lock(Mutex); Pending = Handles; }
			for (const auto& Handle : Pending) Handle->RequestCancel();
			if (!Submitted.exchange(true)) {
				Drive();
				{ std::lock_guard Lock(Mutex); Published = true; }
				Condition.notify_all();
			}
			return Changed;
		}
		auto SetPriority(EShaderBuildPriority Priority) -> void override
		{
			std::vector<std::shared_ptr<FShaderBuildHandle>> Pending;
			{ std::lock_guard Lock(Mutex); Options.Priority = Priority; Pending = Handles; }
			for (const auto& Handle : Pending) Handle->SetPriority(Priority);
		}
		auto IsComplete() const -> bool override
		{
			std::vector<std::shared_ptr<FShaderBuildHandle>> Pending;
			{ std::lock_guard Lock(Mutex); if (!Completed || !Published) return false; Pending = Handles; }
			return std::ranges::all_of(Pending, [](const auto& Handle) { return Handle->IsComplete(); });
		}
		auto Wait() -> EShaderBuildWaitResult override
		{
			if (ShaderGroupCallbackDepth || IsInRenderingThread()) return EShaderBuildWaitResult::WouldBlock;
			size_t Joined = 0;
			for (;;) {
				std::vector<std::shared_ptr<FShaderBuildHandle>> Pending;
				bool Done;
				{ std::unique_lock Lock(Mutex); Condition.wait(Lock, [&] { return Published && (Completed || Handles.size() > Joined); }); Pending = Handles; Done = Completed; }
				for (; Joined < Pending.size(); ++Joined)
					if (Pending[Joined]->Wait() != EShaderBuildWaitResult::Completed) return EShaderBuildWaitResult::WouldBlock;
				if (Done) return EShaderBuildWaitResult::Completed;
			}
		}
		auto FinishJob(size_t Index, uint64 Allowance, FShaderCompilerOutput Output) -> void
		{
			{
				std::lock_guard Lock(Mutex);
				Reserved -= Allowance;
				if (Output) {
					const auto Bytes = RetainedBytes(Output);
					if (Bytes > MaximumBytes - Retained) Output = FShaderCompilerOutput::Failure(std::format("Shader job group exceeds retained-output byte limit: maximum {}, actual {}.", MaximumBytes, Retained + Bytes));
					else Retained += Bytes;
				}
				Outputs[Index] = std::move(Output); --Active;
			}
			Drive();
		}
		auto Drive() -> void
		{
			{ std::lock_guard Lock(Mutex); if (Dispatching || Delivering || Completed) return; Dispatching = true; }
			for (;;) {
				try { if (Options.IsCancelled && Options.IsCancelled()) Canceled = true; }
				catch (...) { Canceled = true; }
				size_t Index;
				uint64 Allowance;
				FShaderBuildRequestOptions RequestOptions;
				FShaderBuildCompletion Callback;
				FShaderCompilerOutput Result{.Status = EShaderBuildStatus::Ok};
				{
					std::lock_guard Lock(Mutex);
					const bool Failed = std::ranges::any_of(Outputs, [](const auto& Output) { return Output && !*Output; });
					if (!Active && (Canceled || Failed || Next == Outputs.size())) {
						Delivering = true; Dispatching = false; Callback = std::move(Completion);
						for (auto& Output : Outputs) {
							if (!Output) continue;
							if (!*Output && Result.Status == EShaderBuildStatus::Ok) Result.Status = Output->Status;
							for (auto& Error : Output->Errors) if (Result.Errors.size() < ShaderCompilerMaxDiagnostics) Result.Errors.push_back(std::move(Error));
							if (*Output) for (auto& Shader : Output->CompiledShaders) Result.CompiledShaders.push_back(std::move(Shader));
						}
						if (!Result) Result.CompiledShaders.clear();
						if (Canceled || Result.Status == EShaderBuildStatus::Canceled) Result = {.Status = EShaderBuildStatus::Canceled};
						Jobs.clear(); Outputs.clear();
					}
					else {
						if (Canceled || Failed || Active == 2 || Next == Outputs.size()) { Dispatching = false; return; }
						Index = Next++; ++Active;
						Allowance = MountedPath.empty() ? std::min(Jobs[Index].MaximumOutputBytes, MaximumBytes - Retained) : MaximumBytes;
						Reserved += Allowance;
						RaisePeak(Manager->PeakActiveJobs, Active); RaisePeak(Manager->PeakOutputBytes, Retained + Reserved);
						RequestOptions = Options;
					}
				}
				if (Callback) {
					++ShaderGroupCallbackDepth;
					try { Callback(std::move(Result)); } catch (...) {}
					--ShaderGroupCallbackDepth;
					std::function<bool()> RetiredObserver;
					std::shared_ptr<const FShaderSourceArtifacts> RetiredSources;
					{ std::lock_guard Lock(Mutex); Completed = true; RetiredObserver = std::move(Options.IsCancelled); RetiredSources = std::move(MountedOptions.SourceArtifacts); }
					Condition.notify_all(); return;
				}
				const auto Observer = std::move(RequestOptions.IsCancelled);
				RequestOptions.IsCancelled = [Weak = weak_from_this(), Observer] {
					const auto Group = Weak.lock(); return !Group || Group->Canceled || (Observer && Observer());
				};
				auto Complete = [Self = shared_from_this(), Index, Allowance](FShaderCompilerOutput Output) { Self->FinishJob(Index, Allowance, std::move(Output)); };
				auto Submitted = [&] {
					if (!MountedPath.empty()) return Backend->CompileMounted(MountedPath, MountedOptions, std::move(RequestOptions), std::move(Complete));
					Jobs[Index].MaximumOutputBytes = Allowance;
					return Backend->CompileGenerated(std::move(Jobs[Index]), std::move(RequestOptions), std::move(Complete));
				}();
				auto Handle = std::make_shared<FShaderBuildHandle>(std::move(Submitted));
				EShaderBuildPriority Priority;
				{ std::lock_guard Lock(Mutex); Handles.push_back(Handle); Priority = Options.Priority; }
				Handle->SetPriority(Priority);
				if (Canceled) Handle->RequestCancel();
				Condition.notify_all();
			}
		}
	};
	auto FShaderCompilingManager::Get() -> FShaderCompilingManager& { static FShaderCompilingManager Manager; return Manager; }
	auto FShaderCompilingManager::SubmitJobs(const FShaderBuildHandle& Jobs) -> void
	{
		const auto Group = std::dynamic_pointer_cast<FJobGroup>(Jobs.Request);
		require(Group && Group->Manager == this);
		if (Group->Submitted.exchange(true)) return;
		require(Group->Completion);
		Group->Backend = IShaderBuildModule::Get();
		std::vector<std::shared_ptr<FJobGroup>> Retired;
		{ std::lock_guard Lock(JobMutex); for (const auto& Item : Groups) if (auto Existing = Item.lock()) Retired.push_back(std::move(Existing)); }
		std::erase_if(Retired, [](const auto& Item) { return !Item->IsComplete(); });
		{
			std::lock_guard Lock(JobMutex);
			if (!Accepting && Group->MaterialCompilingId == 0) Group->Canceled = true;
			std::erase_if(Groups, [&](const auto& Item) { const auto Existing = Item.lock(); return !Existing || std::ranges::contains(Retired, Existing); });
			Groups.push_back(Group);
		}
		const bool ForbiddenInline = Group->Options.bInlineExecution && IsInRenderingThread();
		if (ForbiddenInline || !Group->Backend || !Group->MaximumBytes || Group->Outputs.empty() || Group->Outputs.size() > 128
			|| (Group->MountedPath.empty() && Group->Jobs.empty())) {
			Group->Jobs.clear(); Group->Outputs.clear();
			Group->Outputs.emplace_back(FShaderCompilerOutput::Failure(ForbiddenInline
				? "Synchronous shader compilation is forbidden on the rendering thread."
				: "Shader job input or compiler backend is unavailable."));
			Group->Next = 1;
		}
		Group->Drive();
		{ std::lock_guard Lock(Group->Mutex); Group->Published = true; }
		Group->Condition.notify_all();
	}
	auto FShaderCompilingManager::PrepareJobs(std::vector<FGeneratedShaderCompileRequest> Jobs, uint64 MaximumOutputBytes,
		FShaderBuildRequestOptions Options, FShaderBuildCompletion Completion) -> FShaderBuildHandle
	{
		auto Group = std::make_shared<FJobGroup>(); Group->Manager = this; Group->Outputs.resize(Jobs.size() <= 128 ? Jobs.size() : 0);
		Group->Jobs = std::move(Jobs); Group->MaximumBytes = MaximumOutputBytes;
		Group->Options = std::move(Options); Group->Completion = std::move(Completion);
		require(Group->Completion);
		return FShaderBuildHandle(std::move(Group));
	}
	auto FShaderCompilingManager::PrepareJobs(std::string_view Path, const FShaderCompileOptions& CompileOptions,
		FShaderBuildRequestOptions Options, FShaderBuildCompletion Completion) -> FShaderBuildHandle
	{
		auto Group = std::make_shared<FJobGroup>(); Group->Manager = this; Group->Outputs.resize(1);
		Group->MountedPath = Path; Group->MountedOptions = CompileOptions; Group->MaximumBytes = 256ull * 1024 * 1024;
		Group->Options = std::move(Options); Group->Completion = std::move(Completion);
		require(Group->Completion);
		return FShaderBuildHandle(std::move(Group));
	}
	auto FShaderCompilingManager::CompileGeneratedSync(const FGeneratedShaderCompileRequest& Job) -> FShaderCompilerOutput
	{
		if (IsInRenderingThread())
			return FShaderCompilerOutput::Failure("Synchronous shader compilation is forbidden on the rendering thread.");
		FShaderCompilerOutput Output;
		auto Submitted = PrepareJobs(std::vector{Job}, Job.MaximumOutputBytes, {.bInlineExecution = true}, [&](auto Value) { Output = std::move(Value); });
		SubmitJobs(Submitted);
		require(FinishCompilation(Submitted) == EShaderBuildWaitResult::Completed); return Output;
	}
	auto FShaderCompilingManager::CompileMountedSync(std::string_view Path, const FShaderCompileOptions& Options) -> FShaderCompilerOutput
	{
		if (IsInRenderingThread())
			return FShaderCompilerOutput::Failure("Synchronous shader compilation is forbidden on the rendering thread.");
		const auto Started = std::chrono::steady_clock::now();
		FShaderCompilerOutput Output;
		auto Submitted = PrepareJobs(Path, Options, {.bInlineExecution = true}, [&](auto Value) { Output = std::move(Value); });
		const auto PreparedAt = std::chrono::steady_clock::now();
		SubmitJobs(Submitted);
		const auto SubmittedAt = std::chrono::steady_clock::now();
		require(FinishCompilation(Submitted) == EShaderBuildWaitResult::Completed);
		const auto CompletedAt = std::chrono::steady_clock::now();
		const auto Milliseconds = [](auto Duration) {
			return std::chrono::duration<double, std::milli>(Duration).count();
		};
		const double TotalMilliseconds = Milliseconds(CompletedAt - Started);
		if (TotalMilliseconds >= 100.0)
			DURIN_DEBUG("Slow synchronous mounted shader compile: path={}, total_ms={:.3f}, prepare_ms={:.3f}, submit_ms={:.3f}, finish_ms={:.3f}",
				Path, TotalMilliseconds, Milliseconds(PreparedAt - Started),
				Milliseconds(SubmittedAt - PreparedAt), Milliseconds(CompletedAt - SubmittedAt));
		return Output;
	}
	auto FShaderCompilingManager::FinishCompilation(const FShaderBuildHandle& Handle) -> EShaderBuildWaitResult
	{ Handle.SetPriority(EShaderBuildPriority::High); return Handle.Wait(); }
	auto FShaderCompilingManager::FinishShaderJobs() -> void
	{
		for (;;) {
			std::vector<std::shared_ptr<FJobGroup>> Pending;
			{ std::lock_guard Lock(JobMutex); for (const auto& Item : Groups) if (auto Group = Item.lock()) Pending.push_back(std::move(Group)); }
			std::erase_if(Pending, [](const auto& Group) { return Group->IsComplete(); });
			if (Pending.empty()) return;
			for (const auto& Group : Pending) require(Group->Wait() == EShaderBuildWaitResult::Completed);
		}
	}
	auto FShaderCompilingManager::CancelAllCompilation() -> void
	{
		std::vector<std::shared_ptr<FJobGroup>> Pending;
		{ std::lock_guard Lock(JobMutex); for (const auto& Item : Groups) if (auto Group = Item.lock()) Pending.push_back(std::move(Group)); }
		for (const auto& Group : Pending) Group->RequestCancel();
		CancelMaterialCompilation();
		FinishShaderJobs();
	}
	auto FShaderCompilingManager::GetPeakJobStats() const -> std::pair<uint64, uint64>
	{ return {PeakActiveJobs.load(), PeakOutputBytes.load()}; }

	FShaderCompilingManager::FShaderCompilingManager() = default;
	FShaderCompilingManager::~FShaderCompilingManager() = default;
	auto FShaderCompilingManager::GetNumRemainingShaderJobs() const -> uint64
	{
		std::vector<std::shared_ptr<FJobGroup>> Pending;
		{ std::lock_guard Lock(JobMutex); for (const auto& Item : Groups) if (auto Group = Item.lock()) Pending.push_back(std::move(Group)); }
		return std::ranges::count_if(Pending, [](const auto& Group) { return Group->MaterialCompilingId == 0 && !Group->IsComplete(); });
	}
#if DURIN_WITH_EDITOR
	auto FShaderCompilingManager::SubmitCompileJobs(FShaderMapPtr ShaderMap) -> void
	{
		DerivedData::FRequestBarrier Submission(*ShaderMap->Owner);
		EShaderBuildPriority Priority;
		{ std::lock_guard Lock(ShaderMap->Mutex); Priority = ShaderMap->HighPriority ? EShaderBuildPriority::High : EShaderBuildPriority::Normal; }
		auto Group = std::make_shared<FJobGroup>();
		Group->Manager = &Get(); Group->MaterialCompilingId = ShaderMap->CompilingId;
		Group->Jobs = MakeMaterialShaderStageRequests(*ShaderMap->Translation, ShaderMap->Force);
		Group->Outputs.resize(Group->Jobs.size()); Group->MaximumBytes = MaterialCompileMaxResultBytes;
		Group->Options = {.Priority = Priority, .IsCancelled = [Weak = std::weak_ptr(ShaderMap)] { const auto Value = Weak.lock(); return !Value || IsBuildCanceled(Value); }, .bInlineExecution = ShaderMap->Inline};
		Group->Completion =
			[ShaderMap](FShaderCompilerOutput Output) {
				ShaderMap->Translation->CompileTime = std::chrono::duration<float>(
					std::chrono::steady_clock::now() - ShaderMap->CompilationBegin).count();
				LaunchTask(ShaderMap, [ShaderMap, Output = std::move(Output)]() mutable {
					FMaterialShaderMap::CompleteCompilation(ShaderMap,
						FMaterialShaderMap::FinalizeContent(std::move(*ShaderMap->Translation), std::move(Output), ShaderMap));
				});
			};
		auto Jobs = std::make_shared<FShaderBuildHandle>(std::move(Group));
		ShaderMap->Owner->TrackDependency({
			.IsFinished = [Jobs] { return Jobs->IsComplete(); },
			.Wait = [Jobs] { return Jobs->Wait() == EShaderBuildWaitResult::Completed ? DerivedData::ERequestWaitResult::Completed : DerivedData::ERequestWaitResult::WouldBlock; },
			.RequestCancel = [Jobs] { Jobs->RequestCancel(); }});
		bool HighPriority;
		{ std::lock_guard Lock(ShaderMap->Mutex); ShaderMap->ShaderJobs = Jobs; HighPriority = ShaderMap->HighPriority; }
		if (HighPriority) Jobs->SetPriority(EShaderBuildPriority::High);
		if (IsBuildCanceled(ShaderMap)) Jobs->RequestCancel();
		Get().SubmitJobs(*Jobs);
	}
#endif
}
