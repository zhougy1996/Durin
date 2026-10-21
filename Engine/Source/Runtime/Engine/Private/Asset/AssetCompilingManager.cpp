#include "Asset/AssetCompilingManager.h"
#include "Shader/ShaderCompilingManager.h"
#include "Asset/AssetBuildScheduler.h"

#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "Logging/LogMacros.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Threading/RunnableThread.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCompilingManager.h"
#include "StaticMesh/StaticMeshCompilation.h"
#include "Physics/BodySetup.h"
#include "Physics/PhysicsMeshCompilation.h"

namespace Durin
{
	namespace AssetPrivate
	{
		struct FAssetBuildExecutionState
		{
			uint32 ActiveCount = 0;
			uint64 ScheduledMemory = 0;
			std::mutex Mutex;
			std::condition_variable Changed;
		};
		FAssetBuildExecutionSlot::FAssetBuildExecutionSlot(std::shared_ptr<FAssetBuildExecutionState> InState, uint64 InRequiredMemory)
			: State(std::move(InState)), RequiredMemory(InRequiredMemory)
		{
			std::lock_guard Lock(State->Mutex);
			++State->ActiveCount;
			State->ScheduledMemory += RequiredMemory;
		}
		FAssetBuildExecutionSlot::~FAssetBuildExecutionSlot()
		{
			{
				std::lock_guard Lock(State->Mutex);
				--State->ActiveCount;
				State->ScheduledMemory -= RequiredMemory;
			}
			State->Changed.notify_all();
		}
		FAssetBuildScheduler::FAssetBuildScheduler(uint32 InMaximumConcurrency, uint64 InMemoryBudget)
			: State(std::make_shared<FAssetBuildExecutionState>()), MaximumConcurrency(InMaximumConcurrency), MemoryBudget(InMemoryBudget)
		{ require(MaximumConcurrency > 0); }
		auto FAssetBuildScheduler::Enqueue(bool Interactive, uint64 RequiredMemory,
			std::function<bool(std::shared_ptr<FAssetBuildExecutionSlot>)> Launch, Tasks::FTask Prerequisite, std::function<bool()> IsReady) -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
			Queue.push_back({Interactive, RequiredMemory, std::move(Launch), std::move(Prerequisite), std::move(IsReady)});
			Pump();
		}
		auto FAssetBuildScheduler::Pump() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
			if (bPumping) return;
			bPumping = true;
			struct FResetPump
			{
				bool& Pumping;
				~FResetPump() { Pumping = false; }
			} Reset{bPumping};
			while (!Queue.empty())
			{
				const auto Ready = [](const auto& Item) {
					return (!Item.Prerequisite.IsValid() || Item.Prerequisite.IsCompleted())
						&& (!Item.IsReady || Item.IsReady());
				};
				auto Background = std::ranges::find_if(Queue, [&](const auto& Item) { return !Item.Interactive && Ready(Item); });
				auto Interactive = std::ranges::find_if(Queue, [&](const auto& Item) { return Item.Interactive && Ready(Item); });
				if (Background == Queue.end() && Interactive == Queue.end()) break;
				auto Selected = Interactive != Queue.end() && (Background == Queue.end() || InteractiveBurst < 4) ? Interactive : Background;
				{
					std::lock_guard Lock(State->Mutex);
					if (State->ActiveCount >= MaximumConcurrency) break;
					// Like UE, run one oversized request alone to avoid starvation.
					if (State->ActiveCount && (State->ScheduledMemory >= MemoryBudget
						|| Selected->RequiredMemory > MemoryBudget - State->ScheduledMemory)) break;
				}
				const uint64 RequiredMemory = Selected->RequiredMemory;
				const bool bInteractive = Selected->Interactive;
				auto Launch = std::move(Selected->Launch);
				Queue.erase(Selected); // Dispatch hooks may reenter the scheduler.
				auto Slot = std::shared_ptr<FAssetBuildExecutionSlot>(new FAssetBuildExecutionSlot(State, RequiredMemory));
				// Core destroys accepted callables on body exit or pre-entry cancellation,
				// before publishing terminal state. The callable's capture owns the slot;
				// retaining its task handle or nested DDC dependency does not retain CPU capacity.
				if (Launch(Slot))
				{
					if (bInteractive) ++InteractiveBurst;
					else InteractiveBurst = 0;
				}
			}
		}
		auto FAssetBuildScheduler::WaitForProgress() -> void
		{
			std::unique_lock Lock(State->Mutex);
			State->Changed.wait_for(Lock, std::chrono::milliseconds(1));
		}
		auto FAssetBuildScheduler::GetScheduledMemory() const -> uint64
		{
			std::lock_guard Lock(State->Mutex);
			return State->ScheduledMemory;
		}
		auto GetAssetBuildScheduler() -> std::shared_ptr<FAssetBuildScheduler>
		{
			static auto Scheduler = std::make_shared<FAssetBuildScheduler>();
			return Scheduler;
		}
	}
	namespace
	{
		struct FRegisteredAssetCompiler
		{
			std::vector<DClass*> AssetClasses;
			std::shared_ptr<IAssetCompilingManager> Manager;
			uint64 Generation = 0;
		};

		struct FDispatchEntry
		{
			std::shared_ptr<IAssetCompilingManager> Manager;
			FName CompilerName;
		};

		struct FRoutedBatch
		{
			FDispatchEntry Entry;
			std::vector<DObject*> Objects;
		};

		std::mutex GAssetCompilingMutex;
		std::unordered_map<FName, FRegisteredAssetCompiler> GCompilers;
		std::unordered_map<DClass*, FName> GClassRoutes;
		std::vector<FName> GOrderedCompilers;
		uint64 GNextGeneration = 1;
		uint64 GProcessedCompletions = 0;
		uint64 GRoundRobinCursor = 0;
		bool GRunning = false;
		bool GShutdown = false;
		FAssetPostCompileEvent GPostCompileEvent;


		auto IsCanonicalCompilerName(const FName& Name) -> bool
		{
			const std::string Value = Name.ToString();
			return !Value.empty() && std::ranges::all_of(Value, [](char Character) {
				return Character >= 'a' && Character <= 'z'
					|| Character >= 'A' && Character <= 'Z'
					|| Character >= '0' && Character <= '9'
					|| Character == '.' || Character == '_' || Character == '-';
			});
		}

		auto HasValidUniqueClasses(std::span<DClass* const> Classes) -> bool
		{
			if (Classes.empty()) return false;
			for (size_t Index = 0; Index < Classes.size(); ++Index)
			{
				if (!Classes[Index]) return false;
				if (std::ranges::contains(Classes.first(Index), Classes[Index])) return false;
			}
			return true;
		}

		auto IsSupportedThread() -> bool
		{
			return !GIsGameThreadIdInitialized || IsInGameThread();
		}

		auto SaturatingAdd(uint64 Left, uint64 Right) -> uint64
		{
			return Right > std::numeric_limits<uint64>::max() - Left
				? std::numeric_limits<uint64>::max() : Left + Right;
		}

		auto RebuildCompilerOrder() -> void
		{
			GOrderedCompilers.clear();
			GOrderedCompilers.reserve(GCompilers.size());
			for (const auto& [Name, Entry] : GCompilers) GOrderedCompilers.push_back(Name);
			std::ranges::sort(GOrderedCompilers, {},
				[](const FName& Name) { return Name.ToString(); });
		}

		auto MakeSnapshot(bool bReverse = false) -> std::vector<FDispatchEntry>
		{
			std::vector<FDispatchEntry> Result;
			std::lock_guard Lock(GAssetCompilingMutex);
			Result.reserve(GOrderedCompilers.size());
			for (const FName& Name : GOrderedCompilers)
			{
				const auto It = GCompilers.find(Name);
				if (It == GCompilers.end()) continue;
				Result.push_back({It->second.Manager, Name});
			}
			if (bReverse) std::ranges::reverse(Result);
			return Result;
		}

		auto MakeRoutedBatches(std::span<DObject* const> Objects)
			-> std::vector<FRoutedBatch>
		{
			std::vector<FRoutedBatch> Result;
			std::lock_guard Lock(GAssetCompilingMutex);
			for (DObject* Object : Objects)
			{
				if (!IsValid(Object)) continue;
				FName CompilerName;
				for (DClass* Class = Object->GetClass(); Class; Class = Class->GetSuperClass())
				{
					const auto Route = GClassRoutes.find(Class);
					if (Route != GClassRoutes.end())
					{
						CompilerName = Route->second;
						break;
					}
				}
				if (CompilerName.IsNone()) continue;
				auto Batch = std::ranges::find(Result, CompilerName,
					[](const FRoutedBatch& Item) { return Item.Entry.CompilerName; });
				if (Batch == Result.end())
				{
					const auto Registered = GCompilers.find(CompilerName);
					if (Registered == GCompilers.end()) continue;
					Result.push_back({{Registered->second.Manager, CompilerName}, {}});
					Batch = std::prev(Result.end());
				}
				Batch->Objects.push_back(Object);
			}
			std::ranges::sort(Result, {}, [](const FRoutedBatch& Batch) {
				return Batch.Entry.CompilerName.ToString();
			});
			return Result;
		}

		auto CoalesceSuccessfulAssets(FAssetCompileProcessResult& Destination,
			FAssetCompileProcessResult Source) -> void
		{
			Destination.ProcessedCompletionCount = static_cast<uint32>(std::min<uint64>(
				static_cast<uint64>(Destination.ProcessedCompletionCount)
					+ Source.ProcessedCompletionCount,
				std::numeric_limits<uint32>::max()));
			for (const FWeakObjectPtr& Candidate : Source.SuccessfullyCompiledAssets)
			{
				const FObjectKey Handle = Candidate.GetKey();
				if (IsObjectKeyNull(Handle)) continue;
				const bool bDuplicate = std::ranges::any_of(
					Destination.SuccessfullyCompiledAssets,
					[Handle](const FWeakObjectPtr& Existing) {
						const FObjectKey Other = Existing.GetKey();
						return Handle == Other;
					});
				if (!bDuplicate) Destination.SuccessfullyCompiledAssets.push_back(Candidate);
			}
		}

		auto Publish(const FName& CompilerName, const FAssetCompileProcessResult& Result) -> void
		{
			if (Result.SuccessfullyCompiledAssets.empty()) return;
			FAssetCompileProcessResult Coalesced;
			CoalesceSuccessfulAssets(Coalesced, Result);
			GPostCompileEvent.Broadcast({CompilerName,
				std::move(Coalesced.SuccessfullyCompiledAssets)});
		}

		auto ValidateRegistrationLocked(const FAssetCompilingManagerRegistration& Registration)
			-> FAssetCompilerRegistrationError
		{
			if (!GRunning || GShutdown) return {.Code = EAssetCompilerRegistrationError::NotAccepting, .CompilerName = Registration.Name.ToString()};
			if (GCompilers.contains(Registration.Name)) return {.Code = EAssetCompilerRegistrationError::DuplicateName, .CompilerName = Registration.Name.ToString()};
			for (DClass* Class : Registration.AssetClasses)
				if (GClassRoutes.contains(Class)) return {.Code = EAssetCompilerRegistrationError::DuplicateClass,
					.CompilerName = Registration.Name.ToString(), .AssetClass = Class->GetQualifiedName().ToString()};
			return {};
		}
	}

	FAssetCompilerRegistrationHandle::~FAssetCompilerRegistrationHandle() { Reset(); }
	FAssetCompilerRegistrationHandle::FAssetCompilerRegistrationHandle(
		FAssetCompilerRegistrationHandle&& Other) noexcept
		: CompilerName(Other.CompilerName), Generation(std::exchange(Other.Generation, 0)) {}
	auto FAssetCompilerRegistrationHandle::operator=(
		FAssetCompilerRegistrationHandle&& Other) noexcept
		-> FAssetCompilerRegistrationHandle&
	{
		if (this != &Other)
		{
			Reset();
			CompilerName = Other.CompilerName;
			Generation = std::exchange(Other.Generation, 0);
		}
		return *this;
	}
	auto FAssetCompilerRegistrationHandle::Reset() -> void
	{
		if (Generation == 0) return;
		FAssetCompilingManager::Get().Unregister(CompilerName, Generation);
		Generation = 0;
	}

	auto FAssetCompilingManager::Get() -> FAssetCompilingManager&
	{
		static FAssetCompilingManager Instance;
		return Instance;
	}

	auto FormatAssetCompilerRegistrationError(const FAssetCompilerRegistrationError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EAssetCompilerRegistrationError::None: return {};
		case EAssetCompilerRegistrationError::InvalidRegistration: return "Asset compiler registration is invalid: " + Error.CompilerName;
		case EAssetCompilerRegistrationError::WrongThread: return "Asset compiler registration requires GameThread.";
		case EAssetCompilerRegistrationError::NotAccepting: return "Asset compiling manager is not accepting registrations.";
		case EAssetCompilerRegistrationError::DuplicateName: return "Asset compiler name is already registered: " + Error.CompilerName;
		case EAssetCompilerRegistrationError::DuplicateClass: return "Asset class already has an exact compiler route: " + Error.AssetClass;
		case EAssetCompilerRegistrationError::Start: return FormatAssetCompilerStartError(Error.StartCause);
		}
		return {};
	}

	auto FormatAssetCompilerStartError(const FAssetCompilerStartResult& Result) -> std::string
	{
		switch (Result.Error)
		{
		case EAssetCompilerStartError::None: return {};
		case EAssetCompilerStartError::SchedulerUnavailable: return "Asset compilation requires a running task scheduler.";
		case EAssetCompilerStartError::Undrained: return std::format("Asset compiler still owns {} pending requests.", Result.PendingRequests);
		case EAssetCompilerStartError::StateUnavailable: return "Asset compiler state is unavailable.";
		}
		return {};
	}

	auto FAssetCompilingManager::Start() -> void
	{
		requiref(IsSupportedThread(), "Asset compiling manager must start on GameThread.");
		std::lock_guard Lock(GAssetCompilingMutex);
		requiref(!GShutdown, "Asset compiling manager cannot restart after terminal shutdown.");
		GRunning = true;
	}

	auto FAssetCompilingManager::RegisterCompiler(
		FAssetCompilingManagerRegistration Registration)
		-> FAssetCompilerRegistrationResult
	{
		if (!Registration.Manager || !IsCanonicalCompilerName(Registration.Name)
			|| !HasValidUniqueClasses(Registration.AssetClasses))
		{
			return {.Error = {.Code = EAssetCompilerRegistrationError::InvalidRegistration, .CompilerName = Registration.Name.ToString()}};
		}
		if (!IsSupportedThread())
		{
			return {.Error = {.Code = EAssetCompilerRegistrationError::WrongThread, .CompilerName = Registration.Name.ToString()}};
		}

		{
			std::lock_guard Lock(GAssetCompilingMutex);
			if (auto Error = ValidateRegistrationLocked(Registration); Error.Code != EAssetCompilerRegistrationError::None) return {.Error = std::move(Error)};
		}
		if (const auto Started = Registration.Manager->Start(); !Started)
		{ return {.Error = {.Code = EAssetCompilerRegistrationError::Start, .CompilerName = Registration.Name.ToString(), .StartCause = Started}}; }
		FAssetCompilerRegistrationResult Result;
		{
			std::lock_guard Lock(GAssetCompilingMutex);
			Result.Error = ValidateRegistrationLocked(Registration);
			if (Result.Error.Code == EAssetCompilerRegistrationError::None)
			{
				const uint64 Generation = GNextGeneration++;
				for (DClass* Class : Registration.AssetClasses)
					GClassRoutes.emplace(Class, Registration.Name);
				GCompilers.emplace(Registration.Name, FRegisteredAssetCompiler{
					std::move(Registration.AssetClasses),
					Registration.Manager, Generation});
				RebuildCompilerOrder();
				Result.Handle.CompilerName = Registration.Name;
				Result.Handle.Generation = Generation;
			}
		}
		if (!Result)
		{
			Registration.Manager->StopAdmission();
			Registration.Manager->Shutdown();
			return Result;
		}
		return Result;
	}

	auto FAssetCompilingManager::Unregister(FName CompilerName, uint64 Generation) -> void
	{
		requiref(IsSupportedThread(), "Asset compiler unregistration requires GameThread.");
		FRegisteredAssetCompiler Removed;
		{
			std::lock_guard Lock(GAssetCompilingMutex);
			const auto It = GCompilers.find(CompilerName);
			if (It == GCompilers.end() || It->second.Generation != Generation) return;
			Removed = std::move(It->second);
			for (DClass* Class : Removed.AssetClasses) GClassRoutes.erase(Class);
			GCompilers.erase(It);
			RebuildCompilerOrder();
		}
		Removed.Manager->StopAdmission();
		FAssetCompileProcessResult Result = Removed.Manager->FinishAllCompilation();
		Removed.Manager->Shutdown();
		Publish(CompilerName, Result);
	}

	auto FAssetCompilingManager::ProcessAsyncTasks(bool bLimitExecutionTime) -> FAssetCompileProcessResult
	{
		FAssetCompileProcessParams Params;
		if (bLimitExecutionTime) Params.Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
		return ProcessAsyncTasks(Params);
	}

	auto FAssetCompilingManager::ProcessAsyncTasks(const FAssetCompileProcessParams& Params)
		-> FAssetCompileProcessResult
	{
		FAssetCompileProcessResult Aggregate;
		if (!IsSupportedThread() || Params.MaximumCompletions == 0) return Aggregate;
		auto Entries = MakeSnapshot();
		if (Entries.empty()) return Aggregate;
		std::unordered_map<FName, FAssetCompileProcessResult> CompilerResults;
		auto Accumulate = [&](const FName& CompilerName, FAssetCompileProcessResult Item) {
			CoalesceSuccessfulAssets(CompilerResults[CompilerName], Item);
			CoalesceSuccessfulAssets(Aggregate, std::move(Item));
		};
		const uint64 Cursor = GRoundRobinCursor++;
		for (size_t Offset = 0; Offset < Entries.size(); ++Offset)
		{
			if (Aggregate.ProcessedCompletionCount >= Params.MaximumCompletions) break;
			if (Params.Deadline && std::chrono::steady_clock::now() >= *Params.Deadline) break;
			FDispatchEntry& Entry = Entries[(Offset + Cursor) % Entries.size()];
			const uint32 Remaining = Params.MaximumCompletions
				- Aggregate.ProcessedCompletionCount;
			const uint32 RemainingCompilers = static_cast<uint32>(Entries.size() - Offset);
			FAssetCompileProcessResult Item = Entry.Manager->ProcessAsyncTasks({
				std::max(1u, Remaining / RemainingCompilers), Params.Deadline});
			Accumulate(Entry.CompilerName, std::move(Item));
		}
		while (Aggregate.ProcessedCompletionCount < Params.MaximumCompletions)
		{
			bool bMadeProgress = false;
			for (auto& Entry : Entries)
			{
				if (Aggregate.ProcessedCompletionCount >= Params.MaximumCompletions) break;
				if (Params.Deadline && std::chrono::steady_clock::now() >= *Params.Deadline) break;
				FAssetCompileProcessResult Item = Entry.Manager->ProcessAsyncTasks({
					Params.MaximumCompletions - Aggregate.ProcessedCompletionCount,
					Params.Deadline});
				bMadeProgress |= Item.ProcessedCompletionCount != 0;
				Accumulate(Entry.CompilerName, std::move(Item));
			}
			if (!bMadeProgress) break;
		}
		for (const auto& Entry : Entries)
			if (const auto It = CompilerResults.find(Entry.CompilerName);
				It != CompilerResults.end()) Publish(Entry.CompilerName, It->second);
		{
			std::lock_guard Lock(GAssetCompilingMutex);
			GProcessedCompletions = SaturatingAdd(
				GProcessedCompletions, Aggregate.ProcessedCompletionCount);
		}
		return Aggregate;
	}

	auto FAssetCompilingManager::GetNumRemainingAssets() const -> uint64
	{
		uint64 Count = 0;
		auto Entries = MakeSnapshot();
		for (auto& Entry : Entries)
			Count = SaturatingAdd(Count, Entry.Manager->GetNumRemainingAssets());
		return Count;
	}

	auto FAssetCompilingManager::FinishCompilationForObjects(
		std::span<DObject* const> Objects) -> FAssetCompileProcessResult
	{
		FAssetCompileProcessResult Aggregate;
		if (!IsSupportedThread()) return Aggregate;
		for (FRoutedBatch& Batch : MakeRoutedBatches(Objects))
		{
			FAssetCompileProcessResult Item = Batch.Entry.Manager->FinishCompilationForObjects(Batch.Objects);
			Publish(Batch.Entry.CompilerName, Item);
			CoalesceSuccessfulAssets(Aggregate, std::move(Item));
		}
		return Aggregate;
	}

	auto FAssetCompilingManager::FinishCompilationForObject(DObject& Object)
		-> FAssetCompileProcessResult
	{
		DObject* ObjectPointer = &Object;
		return FinishCompilationForObjects(std::span<DObject* const>(&ObjectPointer, 1));
	}

	auto FAssetCompilingManager::MarkCompilationAsCanceled(
		std::span<DObject* const> Objects) -> void
	{
		if (!IsSupportedThread()) return;
		for (FRoutedBatch& Batch : MakeRoutedBatches(Objects))
			Batch.Entry.Manager->MarkCompilationAsCanceled(Batch.Objects);
	}

	auto FAssetCompilingManager::MarkCompilationAsCanceled(DObject& Object) -> void
	{
		DObject* ObjectPointer = &Object;
		MarkCompilationAsCanceled(std::span<DObject* const>(&ObjectPointer, 1));
	}

	auto FAssetCompilingManager::FinishAllCompilation() -> FAssetCompileProcessResult
	{
		FAssetCompileProcessResult Aggregate;
		if (!IsSupportedThread()) return Aggregate;
		for (;;)
		{
			for (auto& Entry : MakeSnapshot())
			{
				FAssetCompileProcessResult Item = Entry.Manager->FinishAllCompilation();
				Publish(Entry.CompilerName, Item);
				CoalesceSuccessfulAssets(Aggregate, std::move(Item));
			}
			// A completion can submit work to a compiler already visited this pass.
			const auto Remaining = MakeSnapshot();
			if (std::ranges::none_of(Remaining, [](const auto& Entry) { return Entry.Manager->GetNumRemainingAssets() != 0; })) break;
		}
		return Aggregate;
	}

	auto FAssetCompilingManager::GetDiagnostics() const
		-> FAssetCompilingManagerDiagnostics
	{
		FAssetCompilingManagerDiagnostics Result;
		{
			std::lock_guard Lock(GAssetCompilingMutex);
			Result.CompilerCount = static_cast<uint32>(GCompilers.size());
			Result.ProcessedCompletionCount = GProcessedCompletions;
			Result.bAcceptingRequests = GRunning && !GShutdown;
			Result.bShutdown = GShutdown;
		}
		for (auto& Entry : MakeSnapshot())
		{
			const uint64 Remaining = Entry.Manager->GetNumRemainingAssets();
			Result.Compilers.push_back({Entry.CompilerName, Remaining});
			Result.RemainingAssetCount = SaturatingAdd(Result.RemainingAssetCount, Remaining);
		}
		return Result;
	}

	auto FAssetCompilingManager::OnAssetPostCompile() -> FAssetPostCompileEvent&
	{
		return GPostCompileEvent;
	}

	auto FAssetCompilingManager::Shutdown() -> void
	{
		if (!IsSupportedThread()) return;
		{
			std::lock_guard Lock(GAssetCompilingMutex);
			if (GShutdown) return;
			GRunning = false;
			GClassRoutes.clear();
		}
		auto Entries = MakeSnapshot(true);
		for (auto& Entry : Entries)
			Entry.Manager->StopAdmission();
		for (auto& Entry : Entries)
		{
			FAssetCompileProcessResult Item = Entry.Manager->FinishAllCompilation();
			Entry.Manager->Shutdown();
			Publish(Entry.CompilerName, Item);
		}
		std::lock_guard Lock(GAssetCompilingMutex);
		GCompilers.clear();
		GOrderedCompilers.clear();
		GShutdown = true;
	}

	auto FAssetCompilingManager::IsAcceptingRequests() const -> bool
	{
		std::lock_guard Lock(GAssetCompilingMutex);
		return GRunning && !GShutdown;
	}


	auto InitializeAssetCompilingManager() -> FAssetCompilingManagerInitializationResult
	{
		auto& Aggregate = FAssetCompilingManager::Get();
		Aggregate.Start();
		auto ShaderRegistration = Aggregate.RegisterCompiler({
			.Name = FName("Durin.Shader"),
			.AssetClasses = {DMaterial::StaticClass(), DMaterialInstance::StaticClass()},
			.Manager = std::shared_ptr<IAssetCompilingManager>(&FShaderCompilingManager::Get(), [](IAssetCompilingManager*) {})});
		if (!ShaderRegistration)
		{
			DURIN_ERROR("Shader compiling manager failed to register: {}", FormatAssetCompilerRegistrationError(ShaderRegistration.Error));
			Aggregate.Shutdown();
			return {std::move(ShaderRegistration.Error)};
		}
#if DURIN_WITH_EDITORONLY_DATA
		auto TextureRegistration = Aggregate.RegisterCompiler({
			.Name = FName("Durin.Texture"),
			.AssetClasses = {DTexture::StaticClass()},
			.Manager = AssetPrivate::CreateTextureCompilingManager()});
		if (!TextureRegistration)
		{
			DURIN_ERROR("Texture compiling manager failed to register: {}", FormatAssetCompilerRegistrationError(TextureRegistration.Error));
			Aggregate.Shutdown();
			return {std::move(TextureRegistration.Error)};
		}
#endif

		auto StaticMeshRegistration = Aggregate.RegisterCompiler({
			.Name = FName("Durin.StaticMesh"),
			.AssetClasses = {DStaticMesh::StaticClass()},
			.Manager = AssetPrivate::CreateStaticMeshCompilingManager()});
		if (!StaticMeshRegistration)
		{
			DURIN_ERROR("StaticMesh compiling manager failed to register: {}", FormatAssetCompilerRegistrationError(StaticMeshRegistration.Error));
			Aggregate.Shutdown();
			return {std::move(StaticMeshRegistration.Error)};
		}
		auto PhysicsRegistration = Aggregate.RegisterCompiler({
			.Name = FName("Durin.PhysicsMesh"),
			.AssetClasses = {DBodySetup::StaticClass()},
			.Manager = AssetPrivate::CreatePhysicsMeshCompilingManager()});
		if (!PhysicsRegistration)
		{
			DURIN_ERROR("Physics mesh compiling manager failed to register: {}", FormatAssetCompilerRegistrationError(PhysicsRegistration.Error));
			Aggregate.Shutdown();
			return {std::move(PhysicsRegistration.Error)};
		}
		PhysicsRegistration.Handle.Generation = 0;
		StaticMeshRegistration.Handle.Generation = 0;
		ShaderRegistration.Handle.Generation = 0;
#if DURIN_WITH_EDITORONLY_DATA
		TextureRegistration.Handle.Generation = 0;
#endif
		return {};
	}

	auto ShutdownAssetCompilingManager() -> void
	{
		FAssetCompilingManager::Get().Shutdown();
	}
}
