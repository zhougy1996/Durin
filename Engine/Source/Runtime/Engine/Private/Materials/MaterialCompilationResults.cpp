#include "Shader/ShaderCompilingManager.h"
#include "Materials/MaterialShaderMap.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "Materials/ObjectCacheContext.h"
#include "DObject/ObjectLifecycle.h"
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


	}

	auto FShaderCompilingManager::PollMaterialCancellation() -> void
	{
#if DURIN_WITH_EDITOR
		std::vector<FShaderMapPtr> Pending;
		{ std::scoped_lock Lock(MaterialMutex); Pending = CompilingShaderMaps; }
		for (const auto& ShaderMap : Pending) (void)PollCancellation(ShaderMap);
#endif
	}

	auto FShaderCompilingManager::RaiseMaterialPriority(std::span<const FWeakObjectPtr> Owners) -> void
	{
		std::vector<FShaderMapPtr> Pending;
		{
			std::scoped_lock Lock(MaterialMutex);
			if (Owners.empty()) Pending = CompilingShaderMaps;
			else for (const auto& [Resource, Association] : MaterialRequests)
				if (Association.Request && std::ranges::any_of(Owners, [&](auto Owner) { return SameOwner(Resource->Material, Owner); }))
					Pending.push_back(Association.Request);
		}
		for (const auto& ShaderMap : Pending) PrioritizeCompilation(ShaderMap);
	}

	auto FShaderCompilingManager::CacheMaterialShaders(std::shared_ptr<FMaterialCompileRequest> Request) -> void
	{
		CheckMaterialCompileGameThread();
		require(Request->CompilerInput);
		CleanupFinishedShaderMaps();
		// Invalid input and shutdown produce terminal results, never capacity retries.
		const auto Fail = [&](std::string Message) {
			auto Program = std::make_shared<FMaterialCompilerResult>();
			Program->Diagnostics.push_back({.Message = std::move(Message)});
			auto Result = MakeMaterialResult(Request, {.Program = std::move(Program)});
			std::lock_guard Lock(MaterialMutex);
			MaterialRequests.insert_or_assign(Request->Resource, FMaterialRequestAssociation{{}, Request});
			MaterialResults.push_back(std::move(Result));
			++MaterialStatistics.AcceptedRequests; ++MaterialStatistics.FailedRequests; ++OutstandingMaterialConsumers;
		};
		if (!IsAcceptingMaterialRequests()) { Fail("Shader compilation manager is stopped."); return; }
		auto Encoded = MaterialBuildPrivate::EncodeTranslationInput(*Request->CompilerInput);
		if (!Encoded) { Fail("Material translation input could not be encoded."); return; }
		auto TranslationInput = std::make_shared<const FMaterialTranslationInput>(FMaterialTranslationInput{*Encoded, FXxHash128::HashBuffer(Encoded->GetBytes())});
		FBinaryWriter Identity;
		Identity.WriteHash128(TranslationInput->Identity);
		const auto& Environment = Request->CompilerInput->Environment;
		Identity.WriteString(Environment.CompilerIdentity); Identity.WriteString(Environment.Target); Identity.WriteU32(Environment.PassContractVersion);
		auto Dependencies = Environment.Dependencies;
		std::ranges::sort(Dependencies, {}, &FMaterialCompilerDependency::VirtualPath);
		for (const auto& Dependency : Dependencies) { Identity.WriteString(Dependency.VirtualPath); Identity.WriteHash128(Dependency.ContentHash); }
		const FMaterialCompileKey Key{.Identity = {.Digest = FXxHash128::HashBuffer(Identity.GetBytes())}, .bForceRecompile = Request->bForceRecompile};
		FShaderMapPtr NewShaderMap;
		{
			std::lock_guard Lock(MaterialMutex);
			if (const auto Existing = RequestedShaderMaps.find(Key); Existing != RequestedShaderMaps.end() && !Existing->second->Cancellation.IsCanceled())
			{
				Request->bSingleFlightConsumer = true;
				MaterialRequests.insert_or_assign(Request->Resource, FMaterialRequestAssociation{Existing->second, Request});
				Existing->second->CompilingMaterialDependencies.push_back(std::move(Request));
				++MaterialStatistics.AcceptedRequests; ++MaterialStatistics.SingleFlightConsumers; ++OutstandingMaterialConsumers;
				return;
			}
			NewShaderMap = std::make_shared<MaterialBuildPrivate::FMaterialCompilation>(); NewShaderMap->CanonicalIdentity = Key.Identity; NewShaderMap->Force = Key.bForceRecompile;
			MaterialRequests.insert_or_assign(Request->Resource, FMaterialRequestAssociation{NewShaderMap, Request});
			NewShaderMap->CompilingMaterialDependencies.push_back(std::move(Request));
			RequestedShaderMaps.insert_or_assign(Key, NewShaderMap);
			++MaterialStatistics.AcceptedRequests; ++OutstandingMaterialConsumers;
		}
		const auto Input = NewShaderMap->CompilingMaterialDependencies.front()->CompilerInput;
		NewShaderMap->DiagnosticInput = Input;
		const auto Submitted = MaterialBuildPrivate::FMaterialCompilation::Compile(Input,
			{.bForceRecompile = Key.bForceRecompile, .EncodedInput = std::move(TranslationInput), .Request = NewShaderMap},
			[this, Weak = std::weak_ptr(NewShaderMap)](MaterialBuildPrivate::FMaterialBuildResult Build) {
				if (const auto ShaderMap = Weak.lock()) CompleteMaterialRequests(ShaderMap, std::move(Build));
			});
		if (!Submitted) CompleteMaterialRequests(NewShaderMap, {});
	}

	auto FShaderCompilingManager::RegisterMaterialRequest(const std::shared_ptr<FMaterialCompileRequest>& Request) -> void
	{
		CheckMaterialCompileGameThread();
		std::lock_guard Lock(MaterialMutex);
		MaterialRequests.insert_or_assign(Request->Resource, FMaterialRequestAssociation{{}, Request});
		++MaterialStatistics.AcceptedRequests;
		++OutstandingMaterialConsumers;
	}

	auto FShaderCompilingManager::QueueMaterialResult(FMaterialCompileResult Result) -> void
	{
		CheckMaterialCompileGameThread();
		std::lock_guard Lock(MaterialMutex);
		if (Result.Outcome == EMaterialProgramStatus::Ok) ++MaterialStatistics.CompletedRequests;
		else if (Result.Outcome == EMaterialProgramStatus::Error) ++MaterialStatistics.FailedRequests;
		else ++MaterialStatistics.CanceledRequests;
		MaterialResults.push_back(std::move(Result));
	}

	auto FShaderCompilingManager::CancelCompilation(FWeakObjectPtr Owner) -> bool
	{ return CancelCompilation(Owner, true); }

	auto FShaderCompilingManager::CancelCompilation(FWeakObjectPtr Owner, bool bCountCancellation) -> bool
	{
		CheckMaterialCompileGameThread();
		bool Canceled = false;
		std::vector<FShaderMapPtr> Requests;
		{
			std::scoped_lock Lock(MaterialMutex);
			std::vector<std::shared_ptr<FMaterialResource>> Resources;
			for (const auto& [Resource, Association] : MaterialRequests)
				if (SameOwner(Resource->Material, Owner)) Resources.push_back(Resource);
			for (const auto& Resource : Resources)
				Canceled |= DetachMaterialResourceLocked(Resource, Requests, bCountCancellation);
		}
		for (const auto& Request : Requests) CancelCompilation(Request);
		return Canceled;
	}

	auto FShaderCompilingManager::CancelCompilation(const std::shared_ptr<FMaterialResource>& Resource) -> void
	{
		CheckMaterialCompileGameThread();
		std::vector<FShaderMapPtr> Requests;
		{ std::scoped_lock Lock(MaterialMutex); DetachMaterialResourceLocked(Resource, Requests, false); }
		Resource->Compilation.reset();
		for (const auto& Request : Requests) CancelCompilation(Request);
	}


	auto FShaderCompilingManager::ProcessMaterialResults(uint32 MaximumCompletions,
		std::optional<std::chrono::steady_clock::time_point> Deadline, std::vector<FWeakObjectPtr>& CompiledAssets,
		const std::shared_ptr<FMaterialResource>& Resource) -> void
	{
		CheckMaterialCompileGameThread();
		uint32 Consumed = 0;
		while (Consumed < MaximumCompletions && (!Deadline || std::chrono::steady_clock::now() < *Deadline)) {
			FMaterialCompileResult Result;
			{
				std::scoped_lock Lock(MaterialMutex);
				if (MaterialResults.empty()) break;
				const auto Next = Resource ? std::ranges::find_if(MaterialResults, [&](const auto& Item) { return Item.Request->Resource == Resource; }) : MaterialResults.begin();
				if (Next == MaterialResults.end()) break;
				Result = std::move(*Next); MaterialResults.erase(Next);
				const auto Entry = MaterialRequests.find(Result.Request->Resource);
				if (Entry != MaterialRequests.end() && Entry->second.Association == Result.Request) MaterialRequests.erase(Entry);
			}
			try {
				auto* Material = Cast<DMaterialInterface>(Result.Request->Resource->Material.Get());
				FObjectCacheContext Context;
				if (IsValid(Material) && Private::FMaterialCompilationLifecycle::Admit(*Material, std::move(Result), &Context))
					CompiledAssets.emplace_back(Material);
			}
			catch (...) {
				std::scoped_lock Lock(MaterialMutex);
				check(OutstandingMaterialConsumers > 0); --OutstandingMaterialConsumers;
				throw;
			}
			std::scoped_lock Lock(MaterialMutex);
			check(OutstandingMaterialConsumers > 0); --OutstandingMaterialConsumers; ++Consumed;
		}
	}

	auto FShaderCompilingManager::GetMaterialStatistics() const -> FMaterialCompilationStatistics
	{
		std::scoped_lock Lock(MaterialMutex);
		FMaterialCompilationStatistics Result = MaterialStatistics;
		Result.bAcceptingRequests = IsAcceptingMaterialRequests();
		Result.InFlightCount = static_cast<uint32>(std::ranges::count_if(CompilingShaderMaps, [](const auto& Map) { return Map->CanonicalIdentity.IsValid(); }));
		Result.OutstandingConsumerCount = OutstandingMaterialConsumers;
		Result.PendingPublicationCount = static_cast<uint32>(MaterialResults.size());
		const auto Cache = FMaterialShaderMap::GetCacheStats();
		Result.RetainedProgramCount = Cache.first; Result.RetainedProgramBytes = Cache.second;
		return Result;
	}

	auto FShaderCompilingManager::MakeMaterialResult(const std::shared_ptr<FMaterialCompileRequest>& Request,
		const MaterialBuildPrivate::FMaterialBuildResult& Build, const MIR::FCompilerInput* DiagnosticInput, bool Canceled) -> FMaterialCompileResult
	{
		const auto State = Canceled || (Build.Program && Build.Program->Status == EMaterialProgramStatus::Canceled)
			? EMaterialProgramStatus::Canceled : Build ? EMaterialProgramStatus::Ok : EMaterialProgramStatus::Error;
		FMaterialCompileResult Result{
			.Request = Request,
			.StaticProperties = Request->CompilerInput->StaticProperties,
			.Outcome = State};
		for (const auto& Location : Build.Sources) {
			auto Source = Request->CompilerInput->Sources[Location.Ordinal];
			Source.ExpressionIndex = Location.Source.ExpressionIndex;
			Result.ExpressionSources.push_back(std::move(Source));
		}
		if (State == EMaterialProgramStatus::Ok) {
			Result.CompiledProgram = MaterialBuildPrivate::BindMaterialProgramCollections(std::make_shared<const FMaterialProgram>(Build.Program->Resource), Request->CompilerInput->Collections, &Request->CompilerInput->Environment);
			Result.CacheInfo = {.bRetainedHit = Build.RetainedHit,
				.bSharedCompilation = Build.SingleFlight || Request->bSingleFlightConsumer,
				.bForced = Request->bForceRecompile};
		}
		if (State != EMaterialProgramStatus::Canceled && Build.Program) {
			for (auto Diagnostic : Build.Program->Diagnostics) {
				if (Result.Diagnostics.size() >= MaterialProgramMaxDiagnosticCount) break;
				if (Diagnostic.NodeId.IsValid() && DiagnosticInput) {
					const auto Origin = std::ranges::find(DiagnosticInput->Sources, Diagnostic.NodeId, &MIR::FSource::NodeId);
					if (Origin != DiagnosticInput->Sources.end()) {
						const auto& Source = Request->CompilerInput->Sources[std::distance(DiagnosticInput->Sources.begin(), Origin)];
						Diagnostic.NodeId = Source.NodeId; Diagnostic.PortId = Source.PortId;
						Diagnostic.FunctionAssetPath = Source.FunctionAssetPath; Diagnostic.CallPath = Source.CallPath;
					}
				}
				if (Diagnostic.Message.size() > MaterialProgramMaxDiagnosticMessageBytes)
					Diagnostic.Message.resize(MaterialProgramMaxDiagnosticMessageBytes);
				Result.Diagnostics.push_back(std::move(Diagnostic));
			}
		}
		return Result;
	}

	auto FShaderCompilingManager::CompleteMaterialRequests(
		const FShaderMapPtr& ShaderMap,
		MaterialBuildPrivate::FMaterialBuildResult Build) -> void
	{
		std::vector<std::shared_ptr<FMaterialCompileRequest>> Consumers;
		{
			std::scoped_lock Lock(MaterialMutex);
			const auto It = RequestedShaderMaps.find({ShaderMap->CanonicalIdentity, ShaderMap->Force});
			// A canceled map may have been replaced under the same identity. Its
			// remaining consumers still receive their own terminal result.
			if (It != RequestedShaderMaps.end() && It->second == ShaderMap) RequestedShaderMaps.erase(It);
			// Normalize attribution at delivery; asset consumers can join until this lock.
			for (auto& Consumer : ShaderMap->CompilingMaterialDependencies)
				if (Build.SingleFlight && !Consumer->bSingleFlightConsumer) { Consumer->bSingleFlightConsumer = true; ++MaterialStatistics.SingleFlightConsumers; }
			Consumers = std::move(ShaderMap->CompilingMaterialDependencies);
			for (const auto& Consumer : Consumers) ShaderMap->CompletingResources.push_back(Consumer->Resource);
		}
		std::vector<FMaterialCompileResult> Results;
		const bool Canceled = ShaderMap->Cancellation.IsCanceled();
		EMaterialProgramStatus State = EMaterialProgramStatus::Error;
		try {
			if (const auto Hook = MaterialBuildPrivate::ResultFanoutHook.Load()) (*Hook)();
			for (const auto& Consumer : Consumers) {
				auto Result = MakeMaterialResult(Consumer, Build, ShaderMap->DiagnosticInput.get(), Canceled);
				State = Result.Outcome; Results.push_back(std::move(Result));
			}
		}
		catch (...)
		{
			State = EMaterialProgramStatus::Error; Results.clear();
			for (const auto& Consumer : Consumers)
			{
				auto Result = MakeMaterialResult(Consumer, {});
				Result.Diagnostics.push_back({.Message = "Material compile admission was rejected."});
				Results.push_back(std::move(Result));
			}
		}

		{
			std::scoped_lock Lock(MaterialMutex);
			// Detachment wins completion; subsequent cancellation withdraws publication separately.
			if (State == EMaterialProgramStatus::Ok)
			{
				MaterialStatistics.CompletedRequests += Consumers.size();
				if (Build.RetainedHit) MaterialStatistics.RetainedHits += Consumers.size();
			}
			else if (State == EMaterialProgramStatus::Error)
				MaterialStatistics.FailedRequests += Consumers.size();
			else if (State == EMaterialProgramStatus::Canceled)
				MaterialStatistics.CanceledRequests += Consumers.size();

			for (auto& Result : Results)
				if (std::ranges::find(ShaderMap->CompletingResources, Result.Request->Resource) != ShaderMap->CompletingResources.end()) MaterialResults.push_back(std::move(Result));
			ShaderMap->CompletingResources.clear();
		}
	}

	auto FShaderCompilingManager::DetachMaterialResourceLocked(const std::shared_ptr<FMaterialResource>& Resource, std::vector<FShaderMapPtr>& CanceledRequests, bool bCountCancellation) -> bool
	{
		const auto Entry = MaterialRequests.find(Resource);
		if (Entry == MaterialRequests.end()) return false;
		const auto Request = Entry->second.Request;
		Resource->Compilation.reset();
		Resource->CookedPreparation.reset();
		if (bCountCancellation) Resource->Outcome = EMaterialCompileState::Canceled;
		MaterialRequests.erase(Entry);
		if (Request)
		{
			std::erase_if(Request->CompilingMaterialDependencies, [&](const auto& Consumer) { return Consumer->Resource == Resource; });
			std::erase(Request->CompletingResources, Resource);
			if (Request->CompilingMaterialDependencies.empty() && Request->CompletingResources.empty()) CanceledRequests.push_back(Request);
		}
		std::erase_if(MaterialResults, [&](const auto& Result) { return Result.Request->Resource == Resource; });
		check(OutstandingMaterialConsumers > 0);
		--OutstandingMaterialConsumers;
		if (bCountCancellation) ++MaterialStatistics.CanceledRequests;
		return true;
	}
}
