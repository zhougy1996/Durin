#include "MaterialCompilationService.h"
#include "MaterialProgramCompilingManager.h"
#include "Materials/MaterialShaderMap.h"
#include "MaterialTranslationCodec.h"
#include "MaterialCompileTestSupport.h"
#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"
#include "Serialization/BinaryFormat.h"
#include "Templates/AtomicSharedPtr.h"

namespace Durin::MaterialBuildPrivate
{
	static TAtomicSharedPtr<const std::function<void()>> ResultFanoutHook;
	auto SetResultFanoutHookForTests(std::function<void()> Hook) -> void
	{ ResultFanoutHook.Store(Hook ? std::make_shared<const std::function<void()>>(std::move(Hook)) : nullptr); }
}

namespace Durin
{
	auto MapMaterialProgramCategory(EMaterialProgramDiagnosticCategory Category)
		-> EMaterialCompileResultCategory
	{
		switch (Category)
		{
		case EMaterialProgramDiagnosticCategory::Schema:
		case EMaterialProgramDiagnosticCategory::Bounds:
		case EMaterialProgramDiagnosticCategory::Graph:
		case EMaterialProgramDiagnosticCategory::Type:
			return EMaterialCompileResultCategory::Validation;
		case EMaterialProgramDiagnosticCategory::Normalization:
			return EMaterialCompileResultCategory::Normalization;
		case EMaterialProgramDiagnosticCategory::Generation:
			return EMaterialCompileResultCategory::Generation;
		case EMaterialProgramDiagnosticCategory::Dependency:
			return EMaterialCompileResultCategory::Dependency;
		case EMaterialProgramDiagnosticCategory::Compile:
			return EMaterialCompileResultCategory::Compile;
		case EMaterialProgramDiagnosticCategory::Reflection:
			return EMaterialCompileResultCategory::Reflection;
		case EMaterialProgramDiagnosticCategory::Binding:
			return EMaterialCompileResultCategory::Binding;
		}
		return EMaterialCompileResultCategory::Compile;
	}
	namespace
	{
		auto CheckMaterialCompileGameThread() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
		}
		auto SameOwner(FWeakObjectPtr Left, FWeakObjectPtr Right) -> bool
		{
			return Left.GetKey() == Right.GetKey();
		}


		auto EstimateRequestBytes(const FMaterialCompileRequest& Request) -> uint64
		{
			if (!Request.CompilerInput) return 0;
			uint64 Bytes = sizeof(Request) + Request.AssetPath.capacity() + Request.Target.capacity()
				+ MaterialBuildPrivate::EstimateTranslationCaptureBytes(*Request.CompilerInput)
				+ Request.ExpressionSources.capacity() * sizeof(MIR::FSource);
			for (const auto& Source : Request.ExpressionSources) Bytes += Source.FunctionAssetPath.capacity() + Source.CallPath.capacity() * sizeof(FGuid);
			return Bytes;
		}


		auto MakeDiagnostic(
			const FMaterialCompileRequest& Request,
			EMaterialCompileResultCategory Category,
			FMaterialProgramDiagnostic Source) -> FMaterialCompileDiagnostic
		{
			if (Source.Error.ExternalDiagnostic.size() > MaterialProgramMaxDiagnosticMessageBytes)
				Source.Error.ExternalDiagnostic.resize(MaterialProgramMaxDiagnosticMessageBytes);
			std::string AssetPath = Request.AssetPath;
			if (AssetPath.size() > MaterialProgramMaxDiagnosticMessageBytes)
				AssetPath.resize(MaterialProgramMaxDiagnosticMessageBytes);
			return {
				.Category = Category,
				.Source = std::move(Source),
				.AssetPath = std::move(AssetPath),
				.ProgramIdentity = Request.ProgramIdentity,
				.Generation = Request.Generation,
			};
		}

	}

	auto FMaterialCompilationService::Initialize() -> bool
	{
		CheckMaterialCompileGameThread();
		std::scoped_lock Lock(Mutex);
		if (bAcceptingRequests) return true;
		if (!IsTaskSchedulerRunning()) return false;
		bAcceptingRequests = true;
		return true;
	}

	auto FMaterialCompilationService::Shutdown() -> void
	{
		CheckMaterialCompileGameThread();
		std::vector<std::shared_ptr<FMaterialShaderMap>> Pending;
		{
			std::scoped_lock Lock(Mutex);
			bAcceptingRequests = false;
			Pending = Compiles;
			for (auto& Flight : Pending)
			{
				Flight->Cancellation.Cancel();
			}
		}
		for (const auto& Flight : Pending) MaterialBuildPrivate::FMaterialProgramCompilingManager::Get().CancelCompilation(Flight);
		for (const auto& Flight : Pending) MaterialBuildPrivate::FMaterialProgramCompilingManager::Get().FinishCompilation(Flight);
		MaterialBuildPrivate::FMaterialProgramCompilingManager::Get().Shutdown();
		std::scoped_lock Lock(Mutex);
		Diagnostics.CanceledRequests += OutstandingConsumers;
		Flights.clear();
		CompletingFlights.clear(); Compiles.clear(); CapturedBytes = 0;
		Mailbox.clear();
		OutstandingConsumers = 0;
		FMaterialShaderMap::ClearCache();
	}

	auto FMaterialCompilationService::StopAdmission() -> void
	{
		CheckMaterialCompileGameThread();
		std::scoped_lock Lock(Mutex);
		bAcceptingRequests = false;
	}

	auto FMaterialCompilationService::RetireCompiles() -> void
	{
		MaterialBuildPrivate::FMaterialProgramCompilingManager::Get().ProcessAsyncResults();
		std::vector<std::shared_ptr<FMaterialShaderMap>> Retired;
		{ std::scoped_lock Lock(Mutex); Retired = Compiles; }
		std::erase_if(Retired, [](const auto& Flight) { return MaterialBuildPrivate::FMaterialProgramCompilingManager::Get().IsCompiling(Flight); });
		std::scoped_lock Lock(Mutex);
		for (const auto& Flight : Retired)
		{
			const auto It = std::ranges::find(Compiles, Flight);
			if (It != Compiles.end()) { CapturedBytes -= Flight->CapturedBytes; Flight->DiagnosticInput.reset(); Flight->Origins.clear(); Compiles.erase(It); }
		}
	}

	auto FMaterialCompilationService::CancelLatchedCompiles() -> void
	{
		std::vector<std::shared_ptr<FMaterialShaderMap>> Pending;
		{ std::scoped_lock Lock(Mutex); Pending = Compiles; }
		for (const auto& Flight : Pending) if (Flight->Cancellation.IsCancellationRequested()) MaterialBuildPrivate::FMaterialProgramCompilingManager::Get().CancelCompilation(Flight);
	}

	auto FMaterialCompilationService::RaisePendingPriority(std::span<const FWeakObjectPtr> Owners) -> void
	{
		std::vector<std::shared_ptr<FMaterialShaderMap>> Pending;
		{ std::scoped_lock Lock(Mutex); Pending = Compiles; }
		for (const auto& Flight : Pending)
			if (Owners.empty() || std::ranges::any_of(Flight->Origins, [&](auto Origin) { return std::ranges::any_of(Owners, [&](auto Owner) { return SameOwner(Origin, Owner); }); })) MaterialBuildPrivate::FMaterialProgramCompilingManager::Get().PrioritizeCompilation(Flight);
	}

	auto FMaterialCompilationService::FinishAllCompilation() -> void
	{
		std::vector<std::shared_ptr<FMaterialShaderMap>> Pending;
		{ std::scoped_lock Lock(Mutex); Pending = Compiles; }
		for (const auto& Flight : Pending) MaterialBuildPrivate::FMaterialProgramCompilingManager::Get().FinishCompilation(Flight);
		RetireCompiles();
	}

	auto FMaterialCompilationService::FinishCompilation(std::span<const FWeakObjectPtr> Owners) -> void
	{
		std::vector<std::shared_ptr<FMaterialShaderMap>> Pending;
		{ std::scoped_lock Lock(Mutex); Pending = Compiles; }
		for (const auto& Flight : Pending)
			if (std::ranges::any_of(Flight->Origins, [&](const auto& Origin) { return std::ranges::any_of(Owners, [&](const auto& Owner) { return SameOwner(Origin, Owner); }); })) MaterialBuildPrivate::FMaterialProgramCompilingManager::Get().FinishCompilation(Flight);
		RetireCompiles();
	}

	auto FMaterialCompilationService::SubmitJobs(FMaterialCompileRequest Request) -> EMaterialCompileState
	{
		CheckMaterialCompileGameThread();
		RetireCompiles();
		const auto Bytes = EstimateRequestBytes(Request);
		if (!Request.CompilerInput || Bytes > MaterialCompileMaxRequestBytes) return EMaterialCompileState::Rejected;
		struct FCancelOnExit { FMaterialCompilationService& State; ~FCancelOnExit() { State.CancelLatchedCompiles(); } } CancelOnExit{*this};
		// Admission precedes encoding and normalization. Captured encoding is
		// bounded by the input contract and belongs to GameThread snapshot capture.
		{
			std::scoped_lock Lock(Mutex);
			(void)CancelOwnerLocked(Request.Owner, true);
			if (!bAcceptingRequests) return EMaterialCompileState::Rejected;
			if (OutstandingConsumers >= MaterialCompileMaxConsumers || Bytes > 128ull * 1024 * 1024 - CapturedBytes) return EMaterialCompileState::Deferred;
		}
		auto Encoded = MaterialBuildPrivate::EncodeTranslationInput(*Request.CompilerInput);
		if (!Encoded) return EMaterialCompileState::Rejected;
		const auto FlightBytes = Bytes + Encoded->GetRetainedCapacityBytes();
		auto TranslationInput = std::make_shared<const FMaterialTranslationInput>(FMaterialTranslationInput{*Encoded, FXxHash128::HashBuffer(Encoded->GetBytes())});
		FBinaryWriter Identity;
		Identity.WriteHash128(TranslationInput->Identity);
		const auto& Environment = Request.CompilerInput->Environment;
		Identity.WriteString(Environment.CompilerIdentity); Identity.WriteString(Environment.Target); Identity.WriteU32(Environment.PassContractVersion);
		auto Dependencies = Environment.Dependencies;
		std::ranges::sort(Dependencies, {}, &FMaterialCompilerDependency::VirtualPath);
		for (const auto& Dependency : Dependencies) { Identity.WriteString(Dependency.VirtualPath); Identity.WriteHash128(Dependency.ContentHash); }
		const FMaterialCompileKey Key{.Identity = {.Digest = FXxHash128::HashBuffer(Identity.GetBytes())}, .bForceRecompile = Request.bForceRecompile};
		std::shared_ptr<FMaterialShaderMap> NewFlight;
		{
			std::scoped_lock Lock(Mutex);
			if (const auto Existing = Flights.find(Key); Existing != Flights.end())
			{
				if (Existing->second->Cancellation.IsCancellationRequested() || Bytes > 128ull * 1024 * 1024 - CapturedBytes) return EMaterialCompileState::Deferred;
				Request.bSingleFlightConsumer = true;
				if (!std::ranges::any_of(Existing->second->Origins, [&](auto Origin) { return SameOwner(Origin, Request.Owner); })) Existing->second->Origins.push_back(Request.Owner);
				Existing->second->CompilingMaterialDependencies.push_back(std::move(Request)); Existing->second->CapturedBytes += Bytes; CapturedBytes += Bytes;
				++Diagnostics.AcceptedRequests; ++Diagnostics.SingleFlightConsumers; ++OutstandingConsumers;
				return EMaterialCompileState::Running;
			}
			if (FlightBytes > 128ull * 1024 * 1024 - CapturedBytes || Compiles.size() >= MaterialCompileMaxConcurrentRequests) return EMaterialCompileState::Deferred;
			NewFlight = std::make_shared<FMaterialShaderMap>(); NewFlight->CanonicalIdentity = Key.Identity; NewFlight->Force = Key.bForceRecompile;
			NewFlight->CapturedBytes = FlightBytes; CapturedBytes += FlightBytes;
			NewFlight->Origins.push_back(Request.Owner);
			NewFlight->InitialOwner = Request.Owner;
			NewFlight->CompilingMaterialDependencies.push_back(std::move(Request));
			Flights.emplace(Key, NewFlight); Compiles.push_back(NewFlight);
			++Diagnostics.AcceptedRequests; ++OutstandingConsumers;
		}
		const auto Input = NewFlight->CompilingMaterialDependencies.front().CompilerInput;
		NewFlight->DiagnosticInput = Input;
		auto Submitted = MaterialBuildPrivate::FMaterialProgramCompilingManager::Get().SubmitJobs(Input,
			{.bForceRecompile = Key.bForceRecompile, .Cancellation = NewFlight->Cancellation,
				.EncodedInput = std::move(TranslationInput), .ShaderMap = NewFlight},
			[this, Weak = std::weak_ptr(NewFlight)](MaterialBuildPrivate::FMaterialBuildResult Build) {
				const auto Flight = Weak.lock(); if (!Flight) return;
				CompleteFlight(Flight, std::move(Build), Flight->Cancellation.IsCancellationRequested() ? EMaterialCompileState::Canceled : EMaterialCompileState::Ready);
			});
		{
			std::scoped_lock Lock(Mutex);
			if (!Submitted) {
				Flights.erase(Key); std::erase(Compiles, NewFlight);
				CapturedBytes -= NewFlight->CapturedBytes;
				--Diagnostics.AcceptedRequests; --OutstandingConsumers;
				return Submitted.error() == MaterialBuildPrivate::EMaterialCompileAdmissionError::Capacity
					? EMaterialCompileState::Deferred : EMaterialCompileState::Rejected;
			}
			require(*Submitted == NewFlight);
		}
		return EMaterialCompileState::Running;
	}

	auto FMaterialCompilationService::CancelCompilation(FWeakObjectPtr Owner) -> bool
	{
		CheckMaterialCompileGameThread();
		bool Canceled;
		{ std::scoped_lock Lock(Mutex); Canceled = CancelOwnerLocked(Owner, false); }
		CancelLatchedCompiles(); return Canceled;
	}

	auto FMaterialCompilationService::IsCompiling(FWeakObjectPtr Owner) const -> bool
	{
		std::scoped_lock Lock(Mutex);
		for (const auto& [Key, Flight] : Flights)
			if (std::ranges::any_of(Flight->CompilingMaterialDependencies,
				[Owner](const FMaterialCompileRequest& Consumer) {
					return SameOwner(Consumer.Owner, Owner);
				})) return true;
		if (std::ranges::any_of(CompletingFlights, [&](const auto& Flight) { return std::ranges::any_of(Flight->CompletingOwners, [&](auto Pending) { return SameOwner(Pending, Owner); }); })) return true;
		return std::ranges::any_of(Mailbox,
			[Owner](const FMaterialCompileResult& Result) {
				return SameOwner(Result.Owner, Owner);
			});
	}

	auto FMaterialCompilationService::ProcessAsyncResults(uint32 MaximumCount, const std::function<void(FMaterialCompileResult)>& Process) -> uint32
	{
		CheckMaterialCompileGameThread();
		RetireCompiles();
		// A canceled-before-entry body or an exception may never publish its mailbox.
		std::vector<std::shared_ptr<FMaterialShaderMap>> TerminalFlights;
		{
			std::scoped_lock Lock(Mutex);
			for (const auto& [Key, Flight] : Flights)
				if (!MaterialBuildPrivate::FMaterialProgramCompilingManager::Get().IsCompiling(Flight))
					TerminalFlights.push_back(Flight);
		}
		for (const auto& Flight : TerminalFlights)
			CompleteFlight(Flight, {}, Flight->Cancellation.IsCancellationRequested()
				? EMaterialCompileState::Canceled : EMaterialCompileState::Failed);
		std::vector<FMaterialCompileResult> Results;
		{
			std::scoped_lock Lock(Mutex);
			Results.reserve(std::min<size_t>(Mailbox.size(), MaximumCount));
			while (!Mailbox.empty() && Results.size() < MaximumCount)
			{
				Results.push_back(std::move(Mailbox.front()));
				Mailbox.pop_front();
			}
		}
		uint32 Consumed = 0;
		try {
			for (auto& Result : Results) {
				Process(std::move(Result));
				std::scoped_lock Lock(Mutex);
				check(OutstandingConsumers > 0); --OutstandingConsumers; ++Consumed;
			}
		} catch (...) {
			// Gathered results no longer belong to the mailbox, even on publication failure.
			std::scoped_lock Lock(Mutex);
			const auto Remaining = static_cast<uint32>(Results.size()) - Consumed;
			check(OutstandingConsumers >= Remaining); OutstandingConsumers -= Remaining;
			throw;
		}
		return static_cast<uint32>(Results.size());
	}

	auto FMaterialCompilationService::GetDiagnostics() const -> FMaterialCompilationDiagnostics
	{
		std::scoped_lock Lock(Mutex);
		FMaterialCompilationDiagnostics Result = Diagnostics;
		Result.bAcceptingRequests = bAcceptingRequests;
		Result.InFlightCount = static_cast<uint32>(Compiles.size());
		Result.OutstandingConsumerCount = OutstandingConsumers;
		Result.PendingPublicationCount = static_cast<uint32>(Mailbox.size());
		const auto Cache = FMaterialShaderMap::GetCacheStats();
		Result.RetainedProgramCount = Cache.first; Result.RetainedProgramBytes = Cache.second;
		return Result;
	}

	auto FMaterialCompilationService::IsAccepting() const -> bool
	{
		std::scoped_lock Lock(Mutex);
		return bAcceptingRequests;
	}

	auto FMaterialCompilationService::MakeReadyResult(
		const FMaterialCompileRequest& Request,
		std::shared_ptr<const FMaterialCompilerResult> Program,
		EMaterialCompileCacheOutcome CacheOutcome,
		uint64 TaskId) -> FMaterialCompileResult
	{
		Program = FMaterialShaderMap::Configure(std::move(Program), *Request.CompilerInput);
		return {
			.Owner = Request.Owner,
			.AuthoredRevision = Request.AuthoredRevision,
			.Generation = Request.Generation,
			.DependencyRevision = Request.DependencyRevision,
			.ParentChainRevision = Request.ParentChainRevision,
			.ProgramIdentity = Request.ProgramIdentity,
			.StaticProperties = Request.CompilerInput->StaticProperties,
			.Target = Request.Target,
			.State = EMaterialCompileState::Ready,
			.Category = EMaterialCompileResultCategory::None,
			.CacheOutcome = CacheOutcome,
			.TaskId = TaskId,
			.CompiledProgram = std::move(Program),
			.ExpressionSources = Request.ExpressionSources,
		};
	}

	auto FMaterialCompilationService::MakeTerminalResult(const FMaterialCompileRequest& Consumer, EMaterialCompileState State,
		EMaterialCompileResultCategory Category, uint64 TaskId) -> FMaterialCompileResult
	{
		return {.Owner = Consumer.Owner, .AuthoredRevision = Consumer.AuthoredRevision, .Generation = Consumer.Generation,
			.DependencyRevision = Consumer.DependencyRevision, .ParentChainRevision = Consumer.ParentChainRevision,
			.ProgramIdentity = Consumer.ProgramIdentity, .StaticProperties = Consumer.CompilerInput->StaticProperties,
			.Target = Consumer.Target, .State = State, .Category = Category, .TaskId = TaskId, .ExpressionSources = Consumer.ExpressionSources};
	}

	auto FMaterialCompilationService::CompleteFlight(
		const std::shared_ptr<FMaterialShaderMap>& Flight,
		MaterialBuildPrivate::FMaterialBuildResult Build,
		EMaterialCompileState CompletionState) -> void
	{
		std::vector<FMaterialCompileRequest> Consumers;
		{
			std::scoped_lock Lock(Mutex);
			const auto It = Flights.find({Flight->CanonicalIdentity, Flight->Force});
			if (It == Flights.end() || It->second != Flight) return;
			Flights.erase(It);
			// Normalize attribution at delivery; asset consumers can join until this lock.
			const auto Identity = Build.Program ? Build.Program->Identity : FMaterialProgramIdentity{};
			for (auto& Consumer : Flight->CompilingMaterialDependencies) {
				if (Identity.IsValid()) Consumer.ProgramIdentity = Identity;
				Consumer.ExpressionSources.clear();
				for (size_t Index = 0; Index < Build.Sources.size(); ++Index) {
					const auto Ordinal = Build.SourceOrdinals[Index];
					auto Qualified = Consumer.CompilerInput->Sources[Ordinal]; Qualified.ExpressionIndex = Build.Sources[Index].ExpressionIndex;
					Consumer.ExpressionSources.push_back(std::move(Qualified));
				}
				if (Build.SingleFlight && !Consumer.bSingleFlightConsumer) { Consumer.bSingleFlightConsumer = true; ++Diagnostics.SingleFlightConsumers; }
			}
			Consumers = std::move(Flight->CompilingMaterialDependencies);
			for (const auto& Consumer : Consumers) Flight->CompletingOwners.push_back(Consumer.Owner);
			CompletingFlights.push_back(Flight);
		}
		std::vector<FMaterialCompileResult> Results;
		const uint64 TaskId = Build.TaskId;
		const FMaterialCompilerResult Failed;
		const auto& Compiled = Build.Program ? *Build.Program : Failed;
		const auto Override = Build.RetainedHit ? EMaterialCompileCacheOutcome::RetainedHit : EMaterialCompileCacheOutcome::None;
		std::shared_ptr<const FMaterialCompilerResult> Program;
		if (Compiled.Status == EMaterialProgramStatus::Canceled) CompletionState = EMaterialCompileState::Canceled;
		EMaterialCompileState State = CompletionState;
		EMaterialCompileResultCategory Category =
			EMaterialCompileResultCategory::None;
		try
		{
			if (const auto Hook = MaterialBuildPrivate::ResultFanoutHook.Load()) (*Hook)();
			if (CompletionState == EMaterialCompileState::Ready)
			{
				if (!Compiled)
				{
					State = EMaterialCompileState::Failed;
					Category = Compiled.Diagnostics.empty()
						? EMaterialCompileResultCategory::Compile
						: MapMaterialProgramCategory(Compiled.Diagnostics.front().Category);
				}
				else
				{
					Program = Build.Program;
				}
			}
			else if (CompletionState == EMaterialCompileState::Canceled)
				Category = EMaterialCompileResultCategory::Cancellation;
			for (const FMaterialCompileRequest& Consumer : Consumers)
			{
				if (State == EMaterialCompileState::Ready)
				{
					Results.push_back(MakeReadyResult(
						Consumer, Program,
						Override != EMaterialCompileCacheOutcome::None ? Override : Consumer.bSingleFlightConsumer
							? EMaterialCompileCacheOutcome::SingleFlight
							: Flight->Force
							? EMaterialCompileCacheOutcome::Forced
							: EMaterialCompileCacheOutcome::Compiled,
						TaskId));
					continue;
				}

				auto Result = MakeTerminalResult(Consumer, State, Category, TaskId);
				if (State == EMaterialCompileState::Canceled) Result.Diagnostics.clear();
				else
				{
					for (const FMaterialProgramDiagnostic& Diagnostic
						: Compiled.Diagnostics)
					{
						if (Result.Diagnostics.size()
							>= MaterialProgramMaxDiagnosticCount) break;
						auto Qualified = Diagnostic;
						if (Diagnostic.NodeId.IsValid() && Flight->DiagnosticInput && Consumer.CompilerInput)
						{
							const auto Origin = std::ranges::find(Flight->DiagnosticInput->Sources, Diagnostic.NodeId, &MIR::FSource::NodeId);
							if (Origin != Flight->DiagnosticInput->Sources.end())
							{
								const auto Index = std::distance(Flight->DiagnosticInput->Sources.begin(), Origin);
								const auto& Source = Consumer.CompilerInput->Sources[Index];
								Qualified.NodeId = Source.NodeId; Qualified.PortId = Source.PortId; Qualified.FunctionAssetPath = Source.FunctionAssetPath; Qualified.CallPath = Source.CallPath;
							}
						}
						Result.Diagnostics.push_back(MakeDiagnostic(
							Consumer, MapMaterialProgramCategory(Diagnostic.Category),
							std::move(Qualified)));
					}
				}
				Results.push_back(std::move(Result));
			}
		}
		catch (...)
		{
			State = EMaterialCompileState::Failed; Category = EMaterialCompileResultCategory::Compile; Results.clear();
			for (const auto& Consumer : Consumers)
			{
				auto Result = MakeTerminalResult(Consumer, State, Category, TaskId);
				Result.Diagnostics.push_back(MakeDiagnostic(Consumer, Category,
					{.Category = EMaterialProgramDiagnosticCategory::Compile, .Error = EMaterialCompileError::AdmissionRejected}));
				Results.push_back(std::move(Result));
			}
		}

		{
			std::scoped_lock Lock(Mutex);
			// Detachment wins completion; subsequent cancellation withdraws publication separately.
			if (State == EMaterialCompileState::Ready)
			{
				Diagnostics.CompletedRequests += Consumers.size();
				if (Override == EMaterialCompileCacheOutcome::RetainedHit) Diagnostics.RetainedHits += Consumers.size();
			}
			else if (State == EMaterialCompileState::Failed)
				Diagnostics.FailedRequests += Consumers.size();
			else if (State == EMaterialCompileState::Canceled)
				Diagnostics.CanceledRequests += Consumers.size();

			for (auto& Result : Results)
				if (std::ranges::any_of(Flight->CompletingOwners, [&](auto Owner) { return SameOwner(Owner, Result.Owner); })) Mailbox.push_back(std::move(Result));
			Flight->CompletingOwners.clear(); std::erase(CompletingFlights, Flight);
		}
	}

	auto FMaterialCompilationService::CancelOwnerLocked(FWeakObjectPtr Owner, bool bSuperseded) -> bool
	{
		bool bFound = false;
		const auto AccountRemoved = [&](const auto& Flight, size_t Removed, bool Empty) {
			if (Removed == 0) return;
			if (!SameOwner(Owner, Flight->InitialOwner)) std::erase_if(Flight->Origins, [&](auto Origin) { return SameOwner(Origin, Owner); });
			bFound = true; OutstandingConsumers -= static_cast<uint32>(Removed);
			if (bSuperseded) Diagnostics.SupersededRequests += Removed; else Diagnostics.CanceledRequests += Removed;
			if (Empty) Flight->Cancellation.Cancel();
		};
		for (auto& [Key, Flight] : Flights)
		{
			const auto Removed = std::erase_if(Flight->CompilingMaterialDependencies, [Owner](const auto& Consumer) { return SameOwner(Consumer.Owner, Owner); });
			AccountRemoved(Flight, Removed, Flight->CompilingMaterialDependencies.empty());
		}
		for (auto& Flight : CompletingFlights)
		{
			const auto Removed = std::erase_if(Flight->CompletingOwners, [Owner](auto Pending) { return SameOwner(Pending, Owner); });
			AccountRemoved(Flight, Removed, Flight->CompletingOwners.empty());
		}
		for (auto It = Mailbox.begin(); It != Mailbox.end();)
		{
			if (!SameOwner(It->Owner, Owner))
			{
				++It;
				continue;
			}
			bFound = true;
			It = Mailbox.erase(It);
			--OutstandingConsumers;
			if (bSuperseded) ++Diagnostics.SupersededRequests;
			else ++Diagnostics.CanceledRequests;
		}
		return bFound;
	}
}
