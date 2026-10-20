#include "Shader/ShaderCompilingManager.h"
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
			uint64 Bytes = sizeof(Request) + Request.Target.capacity()
				+ MaterialBuildPrivate::EstimateTranslationCaptureBytes(*Request.CompilerInput)
				+ Request.ExpressionSources.capacity() * sizeof(MIR::FSource);
			for (const auto& Source : Request.ExpressionSources) Bytes += Source.FunctionAssetPath.capacity() + Source.CallPath.capacity() * sizeof(FGuid);
			return Bytes;
		}


		auto MakeDiagnostic(FMaterialProgramDiagnostic Source, uint64 Generation) -> FMaterialCompileDiagnostic
		{
			if (Source.Error.ExternalDiagnostic.size() > MaterialProgramMaxDiagnosticMessageBytes)
				Source.Error.ExternalDiagnostic.resize(MaterialProgramMaxDiagnosticMessageBytes);
			return {.Source = std::move(Source), .Generation = Generation};
		}

	}

	auto FShaderCompilingManager::CancelLatchedMaterialCompiles() -> void
	{
		std::vector<std::shared_ptr<FMaterialShaderMap>> Pending;
		{ std::scoped_lock Lock(MaterialMutex); Pending = CompilingShaderMaps; }
		for (const auto& ShaderMap : Pending) if (ShaderMap->Cancellation.IsCancellationRequested()) CancelCompilation(ShaderMap);
	}

	auto FShaderCompilingManager::RaiseMaterialPriority(std::span<const FWeakObjectPtr> Owners) -> void
	{
		std::vector<std::shared_ptr<FMaterialShaderMap>> Pending;
		{
			std::scoped_lock Lock(MaterialMutex);
			for (const auto& ShaderMap : CompilingShaderMaps)
				if (Owners.empty() || std::ranges::any_of(ShaderMap->Origins, [&](auto Origin) {
					return std::ranges::any_of(Owners, [&](auto Owner) { return SameOwner(Origin, Owner); });
				})) Pending.push_back(ShaderMap);
		}
		for (const auto& ShaderMap : Pending) PrioritizeCompilation(ShaderMap);
	}

	auto FShaderCompilingManager::SubmitJobs(FMaterialCompileRequest Request) -> EMaterialCompileState
	{
		CheckMaterialCompileGameThread();
		CleanupFinishedShaderMaps();
		const auto Bytes = EstimateRequestBytes(Request);
		if (!Request.CompilerInput || Bytes > MaterialCompileMaxRequestBytes) return EMaterialCompileState::Rejected;
		struct FCancelOnExit { FShaderCompilingManager& Manager; ~FCancelOnExit() { Manager.CancelLatchedMaterialCompiles(); } } CancelOnExit{*this};
		// Admission precedes encoding and normalization. Captured encoding is
		// bounded by the input contract and belongs to GameThread snapshot capture.
		{
			std::scoped_lock Lock(MaterialMutex);
			(void)CancelMaterialOwnerLocked(Request.Owner, true);
			if (!IsAcceptingMaterialRequests()) return EMaterialCompileState::Rejected;
			if (OutstandingMaterialConsumers >= MaterialCompileMaxConsumers || Bytes > 128ull * 1024 * 1024 - CapturedMaterialBytes) return EMaterialCompileState::Deferred;
		}
		auto Encoded = MaterialBuildPrivate::EncodeTranslationInput(*Request.CompilerInput);
		if (!Encoded) return EMaterialCompileState::Rejected;
		const auto ShaderMapBytes = Bytes + Encoded->GetRetainedCapacityBytes();
		auto TranslationInput = std::make_shared<const FMaterialTranslationInput>(FMaterialTranslationInput{*Encoded, FXxHash128::HashBuffer(Encoded->GetBytes())});
		FBinaryWriter Identity;
		Identity.WriteHash128(TranslationInput->Identity);
		const auto& Environment = Request.CompilerInput->Environment;
		Identity.WriteString(Environment.CompilerIdentity); Identity.WriteString(Environment.Target); Identity.WriteU32(Environment.PassContractVersion);
		auto Dependencies = Environment.Dependencies;
		std::ranges::sort(Dependencies, {}, &FMaterialCompilerDependency::VirtualPath);
		for (const auto& Dependency : Dependencies) { Identity.WriteString(Dependency.VirtualPath); Identity.WriteHash128(Dependency.ContentHash); }
		const FMaterialCompileKey Key{.Identity = {.Digest = FXxHash128::HashBuffer(Identity.GetBytes())}, .bForceRecompile = Request.bForceRecompile};
		std::shared_ptr<FMaterialShaderMap> NewShaderMap;
		{
			std::scoped_lock Lock(MaterialMutex);
			if (const auto Existing = RequestedShaderMaps.find(Key); Existing != RequestedShaderMaps.end())
			{
				if (Existing->second->Cancellation.IsCancellationRequested() || Bytes > 128ull * 1024 * 1024 - CapturedMaterialBytes) return EMaterialCompileState::Deferred;
				Request.bSingleFlightConsumer = true;
				if (!std::ranges::any_of(Existing->second->Origins, [&](auto Origin) { return SameOwner(Origin, Request.Owner); })) Existing->second->Origins.push_back(Request.Owner);
				Existing->second->CompilingMaterialDependencies.push_back(std::move(Request)); Existing->second->CapturedBytes += Bytes; CapturedMaterialBytes += Bytes;
				++MaterialDiagnostics.AcceptedRequests; ++MaterialDiagnostics.SingleFlightConsumers; ++OutstandingMaterialConsumers;
				return EMaterialCompileState::Running;
			}
			if (ShaderMapBytes > 128ull * 1024 * 1024 - CapturedMaterialBytes) return EMaterialCompileState::Deferred;
			NewShaderMap = std::make_shared<FMaterialShaderMap>(); NewShaderMap->CanonicalIdentity = Key.Identity; NewShaderMap->Force = Key.bForceRecompile;
			NewShaderMap->CapturedBytes = ShaderMapBytes; CapturedMaterialBytes += ShaderMapBytes;
			NewShaderMap->Origins.push_back(Request.Owner);
			NewShaderMap->InitialOwner = Request.Owner;
			NewShaderMap->CompilingMaterialDependencies.push_back(std::move(Request));
			RequestedShaderMaps.emplace(Key, NewShaderMap);
			++MaterialDiagnostics.AcceptedRequests; ++OutstandingMaterialConsumers;
		}
		const auto Input = NewShaderMap->CompilingMaterialDependencies.front().CompilerInput;
		NewShaderMap->DiagnosticInput = Input;
		auto Submitted = SubmitJobs(Input,
			{.bForceRecompile = Key.bForceRecompile, .Cancellation = NewShaderMap->Cancellation,
				.EncodedInput = std::move(TranslationInput), .ShaderMap = NewShaderMap},
			[this, Weak = std::weak_ptr(NewShaderMap)](MaterialBuildPrivate::FMaterialBuildResult Build) {
				const auto ShaderMap = Weak.lock(); if (!ShaderMap) return;
				CompleteMaterialRequests(ShaderMap, std::move(Build));
			});
		{
			std::scoped_lock Lock(MaterialMutex);
			if (!Submitted) {
				RequestedShaderMaps.erase(Key);
				CapturedMaterialBytes -= NewShaderMap->CapturedBytes;
				--MaterialDiagnostics.AcceptedRequests; --OutstandingMaterialConsumers;
				return Submitted.error() == MaterialBuildPrivate::EMaterialCompileAdmissionError::Capacity
					? EMaterialCompileState::Deferred : EMaterialCompileState::Rejected;
			}
			require(*Submitted == NewShaderMap);
		}
		return EMaterialCompileState::Running;
	}

	auto FShaderCompilingManager::CancelCompilation(FWeakObjectPtr Owner) -> bool
	{
		CheckMaterialCompileGameThread();
		bool Canceled;
		{ std::scoped_lock Lock(MaterialMutex); Canceled = CancelMaterialOwnerLocked(Owner, false); }
		CancelLatchedMaterialCompiles(); return Canceled;
	}

	auto FShaderCompilingManager::IsMaterialCompiling(FWeakObjectPtr Owner) const -> bool
	{
		std::scoped_lock Lock(MaterialMutex);
		for (const auto& [Key, ShaderMap] : RequestedShaderMaps)
			if (std::ranges::any_of(ShaderMap->CompilingMaterialDependencies,
				[Owner](const FMaterialCompileRequest& Consumer) {
					return SameOwner(Consumer.Owner, Owner);
				})) return true;
		if (std::ranges::any_of(CompilingShaderMaps, [&](const auto& ShaderMap) { return std::ranges::any_of(ShaderMap->CompletingOwners, [&](auto Pending) { return SameOwner(Pending, Owner); }); })) return true;
		return std::ranges::any_of(MaterialResults,
			[Owner](const FMaterialCompileResult& Result) {
				return SameOwner(Result.Owner, Owner);
			});
	}

	auto FShaderCompilingManager::ProcessMaterialResults(uint32 MaximumCount, const std::function<void(FMaterialCompileResult)>& Process) -> uint32
	{
		CheckMaterialCompileGameThread();
		std::vector<FMaterialCompileResult> Results;
		{
			std::scoped_lock Lock(MaterialMutex);
			Results.reserve(std::min<size_t>(MaterialResults.size(), MaximumCount));
			while (!MaterialResults.empty() && Results.size() < MaximumCount)
			{
				Results.push_back(std::move(MaterialResults.front()));
				MaterialResults.pop_front();
			}
		}
		uint32 Consumed = 0;
		try {
			for (auto& Result : Results) {
				Process(std::move(Result));
				std::scoped_lock Lock(MaterialMutex);
				check(OutstandingMaterialConsumers > 0); --OutstandingMaterialConsumers; ++Consumed;
			}
		} catch (...) {
			// Gathered results no longer belong to the mailbox, even on publication failure.
			std::scoped_lock Lock(MaterialMutex);
			const auto Remaining = static_cast<uint32>(Results.size()) - Consumed;
			check(OutstandingMaterialConsumers >= Remaining); OutstandingMaterialConsumers -= Remaining;
			throw;
		}
		return static_cast<uint32>(Results.size());
	}

	auto FShaderCompilingManager::GetMaterialDiagnostics() const -> FMaterialCompilationDiagnostics
	{
		std::scoped_lock Lock(MaterialMutex);
		FMaterialCompilationDiagnostics Result = MaterialDiagnostics;
		Result.bAcceptingRequests = IsAcceptingMaterialRequests();
		Result.InFlightCount = static_cast<uint32>(std::ranges::count_if(CompilingShaderMaps, [](const auto& Map) { return !Map->Origins.empty(); }));
		Result.OutstandingConsumerCount = OutstandingMaterialConsumers;
		Result.PendingPublicationCount = static_cast<uint32>(MaterialResults.size());
		const auto Cache = FMaterialShaderMap::GetCacheStats();
		Result.RetainedProgramCount = Cache.first; Result.RetainedProgramBytes = Cache.second;
		return Result;
	}

	auto FShaderCompilingManager::MakeReadyResult(
		const FMaterialCompileRequest& Request,
		std::shared_ptr<const FMaterialCompilerResult> Program,
		EMaterialCompileCacheOutcome CacheOutcome) -> FMaterialCompileResult
	{
		Program = FMaterialShaderMap::BindCollections(std::move(Program), Request.CompilerInput->Collections, &Request.CompilerInput->Environment);
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
			.CacheOutcome = CacheOutcome,
			.CompiledProgram = std::move(Program),
			.ExpressionSources = Request.ExpressionSources,
		};
	}

	auto FShaderCompilingManager::MakeTerminalResult(const FMaterialCompileRequest& Consumer, EMaterialCompileState State) -> FMaterialCompileResult
	{
		return {.Owner = Consumer.Owner, .AuthoredRevision = Consumer.AuthoredRevision, .Generation = Consumer.Generation,
			.DependencyRevision = Consumer.DependencyRevision, .ParentChainRevision = Consumer.ParentChainRevision,
			.ProgramIdentity = Consumer.ProgramIdentity, .StaticProperties = Consumer.CompilerInput->StaticProperties,
			.Target = Consumer.Target, .State = State, .ExpressionSources = Consumer.ExpressionSources};
	}

	auto FShaderCompilingManager::CompleteMaterialRequests(
		const std::shared_ptr<FMaterialShaderMap>& ShaderMap,
		MaterialBuildPrivate::FMaterialBuildResult Build) -> void
	{
		std::vector<FMaterialCompileRequest> Consumers;
		{
			std::scoped_lock Lock(MaterialMutex);
			const auto It = RequestedShaderMaps.find({ShaderMap->CanonicalIdentity, ShaderMap->Force});
			if (It == RequestedShaderMaps.end() || It->second != ShaderMap) return;
			RequestedShaderMaps.erase(It);
			// Normalize attribution at delivery; asset consumers can join until this lock.
			const auto Identity = Build.Program ? Build.Program->GetShaderMap().ShaderMapId : FMaterialProgramIdentity{};
			for (auto& Consumer : ShaderMap->CompilingMaterialDependencies) {
				if (Identity.IsValid()) Consumer.ProgramIdentity = Identity;
				Consumer.ExpressionSources.clear();
				for (size_t Index = 0; Index < Build.Sources.size(); ++Index) {
					const auto Ordinal = Build.SourceOrdinals[Index];
					auto Qualified = Consumer.CompilerInput->Sources[Ordinal]; Qualified.ExpressionIndex = Build.Sources[Index].ExpressionIndex;
					Consumer.ExpressionSources.push_back(std::move(Qualified));
				}
				if (Build.SingleFlight && !Consumer.bSingleFlightConsumer) { Consumer.bSingleFlightConsumer = true; ++MaterialDiagnostics.SingleFlightConsumers; }
			}
			Consumers = std::move(ShaderMap->CompilingMaterialDependencies);
			for (const auto& Consumer : Consumers) ShaderMap->CompletingOwners.push_back(Consumer.Owner);
		}
		std::vector<FMaterialCompileResult> Results;
		const FMaterialCompilerResult Failed;
		const auto& Compiled = Build.Program ? *Build.Program : Failed;
		const auto Override = Build.RetainedHit ? EMaterialCompileCacheOutcome::RetainedHit : EMaterialCompileCacheOutcome::None;
		EMaterialCompileState State = ShaderMap->Cancellation.IsCancellationRequested() || Compiled.Status == EMaterialProgramStatus::Canceled
			? EMaterialCompileState::Canceled : Compiled ? EMaterialCompileState::Ready : EMaterialCompileState::Failed;
		try
		{
			if (const auto Hook = MaterialBuildPrivate::ResultFanoutHook.Load()) (*Hook)();
			for (const FMaterialCompileRequest& Consumer : Consumers)
			{
				if (State == EMaterialCompileState::Ready)
				{
					Results.push_back(MakeReadyResult(
						Consumer, Build.Program,
						Override != EMaterialCompileCacheOutcome::None ? Override : Consumer.bSingleFlightConsumer
							? EMaterialCompileCacheOutcome::SingleFlight
							: ShaderMap->Force
							? EMaterialCompileCacheOutcome::Forced
							: EMaterialCompileCacheOutcome::Compiled));
					continue;
				}

				auto Result = MakeTerminalResult(Consumer, State);
				if (State != EMaterialCompileState::Canceled)
				{
					for (const FMaterialProgramDiagnostic& Diagnostic
						: Compiled.Diagnostics)
					{
						if (Result.Diagnostics.size()
							>= MaterialProgramMaxDiagnosticCount) break;
						auto Qualified = Diagnostic;
						if (Diagnostic.NodeId.IsValid() && ShaderMap->DiagnosticInput && Consumer.CompilerInput)
						{
							const auto Origin = std::ranges::find(ShaderMap->DiagnosticInput->Sources, Diagnostic.NodeId, &MIR::FSource::NodeId);
							if (Origin != ShaderMap->DiagnosticInput->Sources.end())
							{
								const auto Index = std::distance(ShaderMap->DiagnosticInput->Sources.begin(), Origin);
								const auto& Source = Consumer.CompilerInput->Sources[Index];
								Qualified.NodeId = Source.NodeId; Qualified.PortId = Source.PortId; Qualified.FunctionAssetPath = Source.FunctionAssetPath; Qualified.CallPath = Source.CallPath;
							}
						}
						Result.Diagnostics.push_back(MakeDiagnostic(
							std::move(Qualified), Consumer.Generation));
					}
				}
				Results.push_back(std::move(Result));
			}
		}
		catch (...)
		{
			State = EMaterialCompileState::Failed; Results.clear();
			for (const auto& Consumer : Consumers)
			{
				auto Result = MakeTerminalResult(Consumer, State);
				Result.Diagnostics.push_back(MakeDiagnostic(
					{.Category = EMaterialProgramDiagnosticCategory::Compile, .Error = EMaterialCompileError::AdmissionRejected}, Consumer.Generation));
				Results.push_back(std::move(Result));
			}
		}

		{
			std::scoped_lock Lock(MaterialMutex);
			// Detachment wins completion; subsequent cancellation withdraws publication separately.
			if (State == EMaterialCompileState::Ready)
			{
				MaterialDiagnostics.CompletedRequests += Consumers.size();
				if (Override == EMaterialCompileCacheOutcome::RetainedHit) MaterialDiagnostics.RetainedHits += Consumers.size();
			}
			else if (State == EMaterialCompileState::Failed)
				MaterialDiagnostics.FailedRequests += Consumers.size();
			else if (State == EMaterialCompileState::Canceled)
				MaterialDiagnostics.CanceledRequests += Consumers.size();

			for (auto& Result : Results)
				if (std::ranges::any_of(ShaderMap->CompletingOwners, [&](auto Owner) { return SameOwner(Owner, Result.Owner); })) MaterialResults.push_back(std::move(Result));
			ShaderMap->CompletingOwners.clear();
		}
	}

	auto FShaderCompilingManager::CancelMaterialOwnerLocked(FWeakObjectPtr Owner, bool bSuperseded) -> bool
	{
		bool bFound = false;
		const auto AccountRemoved = [&](const auto& ShaderMap, size_t Removed, bool Empty) {
			if (Removed == 0) return;
			if (!SameOwner(Owner, ShaderMap->InitialOwner)) std::erase_if(ShaderMap->Origins, [&](auto Origin) { return SameOwner(Origin, Owner); });
			bFound = true; OutstandingMaterialConsumers -= static_cast<uint32>(Removed);
			if (bSuperseded) MaterialDiagnostics.SupersededRequests += Removed; else MaterialDiagnostics.CanceledRequests += Removed;
			if (Empty) ShaderMap->Cancellation.Cancel();
		};
		for (auto& [Key, ShaderMap] : RequestedShaderMaps)
		{
			const auto Removed = std::erase_if(ShaderMap->CompilingMaterialDependencies, [Owner](const auto& Consumer) { return SameOwner(Consumer.Owner, Owner); });
			AccountRemoved(ShaderMap, Removed, ShaderMap->CompilingMaterialDependencies.empty());
		}
		for (auto& ShaderMap : CompilingShaderMaps)
		{
			const auto Removed = std::erase_if(ShaderMap->CompletingOwners, [Owner](auto Pending) { return SameOwner(Pending, Owner); });
			AccountRemoved(ShaderMap, Removed, ShaderMap->CompletingOwners.empty());
		}
		for (auto It = MaterialResults.begin(); It != MaterialResults.end();)
		{
			if (!SameOwner(It->Owner, Owner))
			{
				++It;
				continue;
			}
			bFound = true;
			It = MaterialResults.erase(It);
			--OutstandingMaterialConsumers;
			if (bSuperseded) ++MaterialDiagnostics.SupersededRequests;
			else ++MaterialDiagnostics.CanceledRequests;
		}
		return bFound;
	}
}
