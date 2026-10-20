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
			uint64 Bytes = sizeof(Request)
				+ MaterialBuildPrivate::EstimateTranslationCaptureBytes(*Request.CompilerInput);
			return Bytes;
		}


		auto MakeDiagnostic(FMaterialProgramDiagnostic Source, uint64 Generation) -> FMaterialCompileDiagnostic
		{
			if (Source.Error.ExternalDiagnostic.size() > MaterialProgramMaxDiagnosticMessageBytes)
				Source.Error.ExternalDiagnostic.resize(MaterialProgramMaxDiagnosticMessageBytes);
			if (Source.Error.ShaderCause && Source.Error.ShaderCause->ExternalDiagnostic.size() > MaterialProgramMaxDiagnosticMessageBytes) {
				auto Cause = std::make_shared<FShaderError>(*Source.Error.ShaderCause);
				Cause->ExternalDiagnostic.resize(MaterialProgramMaxDiagnosticMessageBytes); Source.Error.ShaderCause = std::move(Cause);
			}
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

	auto FShaderCompilingManager::ProcessMaterialResults(const FAssetCompileProcessParams& Params, const std::function<void(FMaterialCompileResult)>& Process) -> uint32
	{
		CheckMaterialCompileGameThread();
		uint32 Consumed = 0;
		while (Consumed < Params.MaximumCompletions && (!Params.Deadline || std::chrono::steady_clock::now() < *Params.Deadline)) {
			FMaterialCompileResult Result;
			{
				std::scoped_lock Lock(MaterialMutex);
				if (MaterialResults.empty()) break;
				Result = std::move(MaterialResults.front()); MaterialResults.pop_front();
			}
			try { Process(std::move(Result)); }
			catch (...) {
				std::scoped_lock Lock(MaterialMutex);
				check(OutstandingMaterialConsumers > 0); --OutstandingMaterialConsumers;
				throw;
			}
			std::scoped_lock Lock(MaterialMutex);
			check(OutstandingMaterialConsumers > 0); --OutstandingMaterialConsumers; ++Consumed;
		}
		return Consumed;
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

	auto FShaderCompilingManager::MakeMaterialResult(const FMaterialCompileRequest& Request,
		const MaterialBuildPrivate::FMaterialBuildResult& Build, const MIR::FCompilerInput* DiagnosticInput, bool Canceled) -> FMaterialCompileResult
	{
		const auto State = Canceled || (Build.Program && Build.Program->Status == EMaterialProgramStatus::Canceled)
			? EMaterialCompileState::Canceled : Build ? EMaterialCompileState::Ready : EMaterialCompileState::Failed;
		FMaterialCompileResult Result{
			.Owner = Request.Owner, .AuthoredRevision = Request.AuthoredRevision, .Generation = Request.Generation,
			.DependencyRevision = Request.DependencyRevision, .ParentChainRevision = Request.ParentChainRevision,
			.ProgramIdentity = Build.Program ? Build.Program->GetShaderMap().ShaderMapId : FMaterialProgramIdentity{},
			.StaticProperties = Request.CompilerInput->StaticProperties, .Target = Request.CompilerInput->Environment.Target,
			.State = State};
		for (size_t Index = 0; Index < Build.Sources.size(); ++Index) {
			auto Source = Request.CompilerInput->Sources[Build.SourceOrdinals[Index]];
			Source.ExpressionIndex = Build.Sources[Index].ExpressionIndex;
			Result.ExpressionSources.push_back(std::move(Source));
		}
		if (State == EMaterialCompileState::Ready) {
			Result.CompiledProgram = FMaterialShaderMap::BindCollections(Build.Program, Request.CompilerInput->Collections, &Request.CompilerInput->Environment);
			Result.CacheOutcome = Build.RetainedHit ? EMaterialCompileCacheOutcome::RetainedHit
				: Build.SingleFlight || Request.bSingleFlightConsumer ? EMaterialCompileCacheOutcome::SingleFlight
				: Request.bForceRecompile ? EMaterialCompileCacheOutcome::Forced : EMaterialCompileCacheOutcome::Compiled;
		} else if (State != EMaterialCompileState::Canceled && Build.Program) {
			for (auto Diagnostic : Build.Program->Diagnostics) {
				if (Result.Diagnostics.size() >= MaterialProgramMaxDiagnosticCount) break;
				if (Diagnostic.NodeId.IsValid() && DiagnosticInput) {
					const auto Origin = std::ranges::find(DiagnosticInput->Sources, Diagnostic.NodeId, &MIR::FSource::NodeId);
					if (Origin != DiagnosticInput->Sources.end()) {
						const auto& Source = Request.CompilerInput->Sources[std::distance(DiagnosticInput->Sources.begin(), Origin)];
						Diagnostic.NodeId = Source.NodeId; Diagnostic.PortId = Source.PortId;
						Diagnostic.FunctionAssetPath = Source.FunctionAssetPath; Diagnostic.CallPath = Source.CallPath;
					}
				}
				Result.Diagnostics.push_back(MakeDiagnostic(std::move(Diagnostic), Request.Generation));
			}
		}
		return Result;
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
			for (auto& Consumer : ShaderMap->CompilingMaterialDependencies)
				if (Build.SingleFlight && !Consumer.bSingleFlightConsumer) { Consumer.bSingleFlightConsumer = true; ++MaterialDiagnostics.SingleFlightConsumers; }
			Consumers = std::move(ShaderMap->CompilingMaterialDependencies);
			for (const auto& Consumer : Consumers) ShaderMap->CompletingOwners.push_back(Consumer.Owner);
		}
		std::vector<FMaterialCompileResult> Results;
		const bool Canceled = ShaderMap->Cancellation.IsCancellationRequested();
		EMaterialCompileState State = EMaterialCompileState::Failed;
		try {
			if (const auto Hook = MaterialBuildPrivate::ResultFanoutHook.Load()) (*Hook)();
			for (const auto& Consumer : Consumers) {
				auto Result = MakeMaterialResult(Consumer, Build, ShaderMap->DiagnosticInput.get(), Canceled);
				State = Result.State; Results.push_back(std::move(Result));
			}
		}
		catch (...)
		{
			State = EMaterialCompileState::Failed; Results.clear();
			for (const auto& Consumer : Consumers)
			{
				auto Result = MakeMaterialResult(Consumer, {});
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
				if (Build.RetainedHit) MaterialDiagnostics.RetainedHits += Consumers.size();
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
