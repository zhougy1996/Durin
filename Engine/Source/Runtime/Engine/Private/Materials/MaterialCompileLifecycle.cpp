#include "Materials/ObjectCacheContext.h"
#include "Materials/MaterialShaderMap.h"
#include "Shader/ShaderCompilingManager.h"
#include "Shader/ShaderMapPreparation.h"
#include "MaterialCollectionBinding.h"
#include "MaterialCompileRetryQueue.h"
#include "Asset/OfflinePreparation.h"
#include "Asset/Asset.h"
#include "Asset/CookDependencies.h"
#include "DObject/Package.h"
#include "Materials/MaterialCompileLifecycle.h"

#include "Asset/AssetCompilingManager.h"
#include "CoreGlobals.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "Logging/LogMacros.h"
#include "Materials/Material.h"
#include "Shader/ShaderCompilerCore.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		auto CheckMaterialCompileGameThread() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
		}

		auto AdvanceNonzero(uint64 Value) -> uint64
		{
			return Value == std::numeric_limits<uint64>::max() ? 1 : Value + 1;
		}

		auto SameOwner(FWeakObjectPtr Left, FWeakObjectPtr Right) -> bool
		{ return Left.GetKey() == Right.GetKey(); }

		std::atomic_uint8_t GPendingShaderReloadMode = 0;
		auto CancelMaterialCompileDomain(DMaterialInterface& Material) -> bool;

	}

	namespace Private
	{
		auto GetMaterialCompileRetryQueue() -> FMaterialCompileRetryQueue&
		{
			CheckMaterialCompileGameThread();
			static FMaterialCompileRetryQueue Queue;
			return Queue;
		}

		auto FMaterialCompilationLifecycle::ScheduleEdit(DMaterialInterface& Material, FObjectCacheContext* Context) -> void
		{
			CheckMaterialCompileGameThread();
#if DURIN_WITH_EDITORONLY_DATA
			if (Material.IsDynamicInstance()) return;
			if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) return;
			DMaterialInterface* Root = &Material;
			for (uint32 Depth = 0; Root && Root->GetParent() && Depth < MaterialMaximumParentDepth; ++Depth)
				Root = Root->GetParent();
			const auto* Base = Cast<DMaterial>(Root);
			const auto Mode = Base ? Base->GetEditCompileMode() : EMaterialEditCompileMode::Immediate;
			if (Mode == EMaterialEditCompileMode::Immediate)
			{
				RequestCurrent(Material, false, Context);
				return;
			}
			CancelMaterialCompileDomain(Material);
			Material.CompilationOwner.LastObservedShaderProperties =
				CanonicalizeMaterialShaderProperties(Material.GetStaticProperties());
			auto& Declarations = Material.CompilationOwner.LastObservedParameters;
			Declarations.clear();
			for (const auto& Definition : Material.GetParameterDefinitions())
				Declarations.push_back({Definition.Id, Definition.Type});
			std::ranges::sort(Declarations, {}, &FMaterialCompilerParameterDeclaration::Id);
			auto& Status = Material.CompilationOwner.MaterialCompileStatus;
			Status.State = Mode == EMaterialEditCompileMode::Automatic
				? EMaterialCompileState::Scheduled : EMaterialCompileState::NeedsCompile;
			Status.CacheOutcome = EMaterialCompileCacheOutcome::None;
			Material.CompilationOwner.MaterialCompileDiagnostics.clear();
			Material.CompilationOwner.bDeferredForceRecompile = false;
			Material.CompilationOwner.EditCompileDeadline =
				std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
			if (Status.State == EMaterialCompileState::Scheduled)
				GetMaterialCompileRetryQueue().Add(FWeakObjectPtr(&Material));
#endif
		}

		auto FMaterialCompilationLifecycle::Submit(
			DMaterialInterface& Material,
			MIR::FCompilerInput Input,
			bool bForceRecompile, std::vector<FMaterialFunctionOwnerStamp> FunctionOwners, FObjectCacheContext* Context) -> bool
		{
			CheckMaterialCompileGameThread();
			if (Material.IsDynamicInstance()) return false;
			Private::GetMaterialCompileRetryQueue().Remove(FWeakObjectPtr(&Material));
			auto& Compilation = FShaderCompilingManager::Get();
			// Construction precedes DObject handle registration. In a running engine,
			// defer that bootstrap request to PostLoad, an authored edit, or an
			// explicit request instead of performing expensive work on GameThread.
			if (FWeakObjectPtr(&Material).GetKey().IsNull()
				&& Compilation.IsAcceptingMaterialRequests())
			{
				Material.CompilationOwner.MaterialCompileStatus.State =
					EMaterialCompileState::NeverRequested;
				return true;
			}
			Material.CompilationOwner.MaterialCompileStatus.State = EMaterialCompileState::Running;
			Material.CompilationOwner.RequestedFunctionOwners = std::move(FunctionOwners);
#if DURIN_WITH_EDITOR
			if (Compilation.IsAcceptingMaterialRequests() && !FScopedOfflinePreparation::IsActive())
			{
				auto& Status = Material.CompilationOwner.MaterialCompileStatus;
				Status.RequestGeneration = AdvanceNonzero(Status.RequestGeneration);
				Status.DependencyRevision = GetShaderReloadGeneration(); Status.Target = Input.Environment.Target;
				Status.RequestedIdentity = {}; Status.CacheOutcome = EMaterialCompileCacheOutcome::None;
				Material.CompilationOwner.RequestedExpressionSources = Input.Sources;
				Material.CompilationOwner.MaterialCompileDiagnostics.clear();
				FMaterialCompileRequest Request{
					.Owner = FWeakObjectPtr(&Material), .AuthoredRevision = Status.AuthoredRevision,
					.Generation = Status.RequestGeneration, .DependencyRevision = Status.DependencyRevision,
					.ParentChainRevision = Status.ParentChainRevision,
					.CompilerInput = std::make_shared<const MIR::FCompilerInput>(std::move(Input)),
					.bForceRecompile = bForceRecompile};
				const auto Submitted = Compilation.SubmitJobs(std::move(Request));
				Status.State = Submitted;
				if (Submitted == EMaterialCompileState::Deferred)
				{
					Material.CompilationOwner.bDeferredForceRecompile = bForceRecompile;
					GetMaterialCompileRetryQueue().Add(FWeakObjectPtr(&Material));
					return true;
				}
				if (Submitted == EMaterialCompileState::Rejected)
				{
					Material.RetireFailedMaterialGeneration(Context); return false;
				}
				return true;
			}
#endif
			auto& Status = Material.CompilationOwner.MaterialCompileStatus;
			Status.RequestGeneration = AdvanceNonzero(Status.RequestGeneration);
			Status.DependencyRevision = GetShaderReloadGeneration(); Status.Target = Input.Environment.Target;
			Status.CacheOutcome = EMaterialCompileCacheOutcome::None;
			Material.CompilationOwner.MaterialCompileDiagnostics.clear();
			auto Build = MaterialBuildPrivate::CompileMaterialInput(Input, bForceRecompile);
			FMaterialCompileRequest Request{
				.Owner = FWeakObjectPtr(&Material), .AuthoredRevision = Status.AuthoredRevision,
				.Generation = Status.RequestGeneration, .DependencyRevision = Status.DependencyRevision,
				.ParentChainRevision = Status.ParentChainRevision,
				.CompilerInput = std::shared_ptr<const MIR::FCompilerInput>(&Input, [](const auto*) {}),
				.bForceRecompile = bForceRecompile};
			Material.CompilationOwner.RequestedExpressionSources = Input.Sources;
			Status.RequestedIdentity = {};
			auto Result = FShaderCompilingManager::MakeMaterialResult(Request, Build);
			Admit(Material, std::move(Result), Context);
			return Status.State == EMaterialCompileState::Ready;
		}

		auto FMaterialCompilationLifecycle::Admit(
			DMaterialInterface& Material, FMaterialCompileResult Result, FObjectCacheContext* Context) -> bool
		{
				CheckMaterialCompileGameThread();
				FMaterialCompileStatus& Status = Material.CompilationOwner.MaterialCompileStatus;
				if (Status.HasUnsubmittedEdits()) return false;
				if (Result.Generation != Status.RequestGeneration
					|| Result.AuthoredRevision != Status.AuthoredRevision
					|| Result.DependencyRevision != Status.DependencyRevision
					|| Result.ParentChainRevision != Status.ParentChainRevision
					|| Result.Target != Status.Target)
				{
					return false;
				}

				if (Result.DependencyRevision != GetShaderReloadGeneration())
				{ RequestCurrent(Material, false, Context); return false; }

				const auto OwnerHandle = FWeakObjectPtr(&Material);
				if (!OwnerHandle.GetKey().IsNull())
				{
					if (!SameOwner(Result.Owner, OwnerHandle)) return false;
					FResolvedMaterialProperties Resolved;
					const auto Error = ResolveMaterialProperties(Material, Resolved);
					if (!Error)
					{
						RequestCurrent(Material, false, Context);
						return false;
					}
				}

				if (!AreMaterialFunctionOwnersCurrent(Material.CompilationOwner.RequestedFunctionOwners))
				{
					Status.AuthoredRevision = AdvanceNonzero(Status.AuthoredRevision);
					ScheduleEdit(Material, Context);
					return false;
				}
				// Detached translation supplies the identity for this qualified generation.
				if (!Status.RequestedIdentity.IsValid()) Status.RequestedIdentity = Result.ProgramIdentity;
				if (!Result.ExpressionSources.empty()) Material.CompilationOwner.RequestedExpressionSources = std::move(Result.ExpressionSources);
				const bool SourceChanged = std::ranges::any_of(Result.Diagnostics, [](const auto& Diagnostic) {
					const auto* Error = std::get_if<EMaterialCompileError>(&Diagnostic.Source.Error.Code);
					return Error && *Error == EMaterialCompileError::SourceDependencyChanged;
				});
				if (SourceChanged && Status.SourceRetryCount == 0)
				{
					++Status.SourceRetryCount; AdvanceShaderReloadGeneration();
					RequestCurrent(Material, false, Context); return false;
				}

				Status.State = Result.State;
				Status.CacheOutcome = Result.CacheOutcome;
				Material.CompilationOwner.MaterialCompileDiagnostics = std::move(Result.Diagnostics);
				if (Result.State == EMaterialCompileState::Ready
					&& Result.CompiledProgram
					&& Result.ProgramIdentity == Status.RequestedIdentity
					&& Result.CompiledProgram->GetShaderMap().ShaderMapId == Result.ProgramIdentity
					&& ValidateMaterialCompilerResult(*Result.CompiledProgram))
				{
					FMaterialLocalRenderLayer Candidate;
					Candidate.CompiledProgram = Result.CompiledProgram;
					Status.SourceRetryCount = 0;
					if (!Candidate.CompiledProgram->ActiveCollections.empty())
					{
						auto Current = MaterialBuildPrivate::ResolvePublicationCollections(Candidate.CompiledProgram->ActiveCollections);
						if (!Current) { ScheduleEdit(Material, Context); return false; }
						if (auto Refreshed = FMaterialShaderMap::BindCollections(Candidate.CompiledProgram, *Current)) Candidate.CompiledProgram = std::move(Refreshed);
					}
					const auto ShaderProperties = CanonicalizeMaterialShaderProperties(Result.StaticProperties);
					Candidate.StaticProperties = Material.GetStaticProperties();
					Candidate.StaticProperties->OpacityMaskThreshold = ShaderProperties.OpacityMaskThreshold;
					if (CanonicalizeMaterialShaderProperties(*Candidate.StaticProperties) != ShaderProperties)
					{
						Status.State = EMaterialCompileState::Superseded;
						return false;
					}
					for (const auto& Parameter : Candidate.CompiledProgram->GetShaderMap().ActiveParameters)
					{
						FResolvedMaterialParameter Resolved;
						if (!Material.ResolveParameterValue(Parameter.Id, Resolved)
							|| !Resolved.Definition || Resolved.Definition->Type != Parameter.Type)
						{
							Status.State = EMaterialCompileState::Rejected;
							Material.RetireFailedMaterialGeneration(Context);
							return false;
						}
						Candidate.Parameters.push_back(BuildMaterialLocalRenderParameter(
							Parameter.Id, Resolved.Value));
					}
					Material.CompilationOwner.RenderLayer = std::move(Candidate);
					Material.CompilationOwner.AcceptedExpressionSources = Material.CompilationOwner.RequestedExpressionSources;
					Status.CompiledIdentity = Result.ProgramIdentity;
					Status.DurationMicroseconds =
						Material.CompilationOwner.RenderLayer.CompiledProgram->Timings.NormalizationMicroseconds
						+ Material.CompilationOwner.RenderLayer.CompiledProgram->Timings.GenerationMicroseconds
						+ Material.CompilationOwner.RenderLayer.CompiledProgram->Timings.CompilationMicroseconds;
					Material.MarkRenderDataDirty(
						EMaterialRenderDirtyFlags::ShaderMap
							| EMaterialRenderDirtyFlags::PipelineState, false, Context);
					return true;
				}

				if (Result.State == EMaterialCompileState::Ready)
				{
					Status.State = EMaterialCompileState::Rejected;
				}
				if (Status.State == EMaterialCompileState::Failed
					|| Status.State == EMaterialCompileState::Rejected)
					Material.RetireFailedMaterialGeneration(Context);
				return false;
		}

		auto FMaterialCompilationLifecycle::MarkCanceled(
			DMaterialInterface& Material) -> void
		{
			GetMaterialCompileRetryQueue().Remove(FWeakObjectPtr(&Material));
			Material.CompilationOwner.MaterialCompileStatus.State = EMaterialCompileState::Canceled;
		}

		auto FMaterialCompilationLifecycle::RetryDeferred(DMaterialInterface& Material) -> void
		{
			if (Material.GetMaterialCompileStatus().State == EMaterialCompileState::Scheduled
				&& std::chrono::steady_clock::now() < Material.CompilationOwner.EditCompileDeadline) return;
			RequestCurrent(Material, Material.CompilationOwner.bDeferredForceRecompile);
		}

		auto FMaterialCompilationLifecycle::RequestCurrent(
			DMaterialInterface& Material, bool bForceRecompile, FObjectCacheContext* Context) -> bool
		{
			if (Material.IsDynamicInstance()) return false;
			GetMaterialCompileRetryQueue().Remove(FWeakObjectPtr(&Material));
			if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) return false;
			FResolvedMaterialProperties Resolved;
			const auto Error = ResolveMaterialProperties(Material, Resolved);
			if (!Error)
			{
				Material.CompilationOwner.MaterialCompileStatus.RequestGeneration = AdvanceNonzero(
					Material.CompilationOwner.MaterialCompileStatus.RequestGeneration);
				Material.CompilationOwner.MaterialCompileStatus.State = EMaterialCompileState::Failed;
				Material.CompilationOwner.MaterialCompileDiagnostics = {{
					.Source = {.Category = EMaterialProgramDiagnosticCategory::Dependency,
						.Error = Error.Error.HasError() ? Error.Error : FMaterialError(EMaterialCompileError::NoAuthoredProgram)},
					.Generation = Material.CompilationOwner.MaterialCompileStatus.RequestGeneration}};
				Material.RetireFailedMaterialGeneration(Context);
				return false;
			}
			return Material.RequestProgramCompile(
				Material.GetStaticProperties(), bForceRecompile, Context);
		}
	}

	auto IsMaterialCompilationAcceptingRequests() -> bool
	{
		return FShaderCompilingManager::Get().IsAcceptingMaterialRequests();
	}

	auto HasPendingMaterialCompilation(const DMaterialInterface& Material) -> bool
	{
		CheckMaterialCompileGameThread();
		const auto State = Material.GetMaterialCompileStatus().State;
		return FShaderCompilingManager::Get().IsMaterialCompiling(FWeakObjectPtr(const_cast<DMaterialInterface*>(&Material)))
			|| State == EMaterialCompileState::Deferred || State == EMaterialCompileState::Scheduled;
	}

	auto GetMaterialCompilationDiagnostics()
		-> FMaterialCompilationDiagnostics
	{
		return FShaderCompilingManager::Get().GetMaterialDiagnostics();
	}

	auto NotifyMaterialShaderReload(bool bForceRecompile) -> void
	{
		uint8 Desired = bForceRecompile ? 2 : 1;
		uint8 Current = GPendingShaderReloadMode.load(std::memory_order_acquire);
		while (Current < Desired
			&& !GPendingShaderReloadMode.compare_exchange_weak(
				Current, Desired, std::memory_order_acq_rel)) {}
	}

	auto RequestMaterialRecompile(
		DMaterialInterface& Material, bool bForceRecompile) -> bool
	{
		return Private::FMaterialCompilationLifecycle::RequestCurrent(
			Material, bForceRecompile);
	}

	namespace
	{
		auto CancelMaterialCompileDomain(DMaterialInterface& Material) -> bool
		{
			CheckMaterialCompileGameThread();
			Private::GetMaterialCompileRetryQueue().Remove(FWeakObjectPtr(&Material));
			const bool bCanceled = FShaderCompilingManager::Get().CancelCompilation(FWeakObjectPtr(&Material));
			if (bCanceled || Material.GetMaterialCompileStatus().State == EMaterialCompileState::Deferred
				|| Material.GetMaterialCompileStatus().State == EMaterialCompileState::Scheduled)
				Private::FMaterialCompilationLifecycle::MarkCanceled(Material);
			return bCanceled;
		}
	}

	auto FShaderCompilingManager::Start() -> FAssetCompilerStartResult
	{
		if (!IsTaskSchedulerRunning()) return {EAssetCompilerStartError::SchedulerUnavailable};
		Accepting = true; return {};
	}
	auto FShaderCompilingManager::StopAdmission() -> void
	{
		{ std::lock_guard Lock(JobMutex); Accepting = false; }
		auto& RetryQueue = Private::GetMaterialCompileRetryQueue();
		RetryQueue.Process(RetryQueue.Num(), [](FWeakObjectPtr Owner) {
			if (auto* Material = Cast<DMaterialInterface>(Owner.Get()); IsValid(Material))
				Private::FMaterialCompilationLifecycle::MarkCanceled(*Material);
			return false;
		});
	}
	auto FShaderCompilingManager::GetNumRemainingAssets() const -> uint64
	{
		return GetMaterialDiagnostics().OutstandingConsumerCount
			+ Private::GetMaterialCompileRetryQueue().Num()
			+ GetNumRemainingShaderJobs() + GetNumRemainingMaterialShaderMaps();
	}
	auto FShaderCompilingManager::ProcessAsyncTasks(const FAssetCompileProcessParams& Params)
		-> FAssetCompileProcessResult
	{ return ProcessAsyncResults(Params); }
	auto FShaderCompilingManager::FinishCompilationForObjects(std::span<DObject* const> Objects)
		-> FAssetCompileProcessResult
	{
		FAssetCompileProcessResult Aggregate;
		std::vector<FWeakObjectPtr> Owners;
		for (DObject* Object : Objects)
			if (auto* Material = Cast<DMaterialInterface>(Object); IsValid(Material))
			{
				if (Material->GetMaterialCompileStatus().State == EMaterialCompileState::Scheduled)
					Private::FMaterialCompilationLifecycle::RequestCurrent(*Material, false);
				Owners.push_back(FWeakObjectPtr(Material));
			}
		while (std::ranges::any_of(Owners, [this](FWeakObjectPtr Owner) {
			auto* Material = Cast<DMaterialInterface>(Owner.Get());
			return IsMaterialCompiling(Owner) || (IsValid(Material)
				&& Material->GetMaterialCompileStatus().State == EMaterialCompileState::Deferred);
		}))
		{
			RaiseMaterialPriority(Owners);
			auto Item = ProcessAsyncResults({.MaximumCompletions = std::numeric_limits<uint32>::max()});
			Aggregate.ProcessedCompletionCount += Item.ProcessedCompletionCount;
			Aggregate.SuccessfullyCompiledAssets.insert(
				Aggregate.SuccessfullyCompiledAssets.end(),
				Item.SuccessfullyCompiledAssets.begin(),
				Item.SuccessfullyCompiledAssets.end());
			if (Item.ProcessedCompletionCount == 0) std::this_thread::yield();
		}
		FinishMaterialCompilation(Owners);
		return Aggregate;
	}
	auto FShaderCompilingManager::MarkCompilationAsCanceled(std::span<DObject* const> Objects)
		-> void
	{
		for (DObject* Object : Objects)
			if (auto* Material = Cast<DMaterialInterface>(Object); IsValid(Material))
				CancelMaterialCompileDomain(*Material);
	}
	auto FShaderCompilingManager::ProcessAsyncResults(const FAssetCompileProcessParams& Params) -> FAssetCompileProcessResult
	{
		CheckMaterialCompileGameThread();
		if (!Params.MaximumCompletions || (Params.Deadline && std::chrono::steady_clock::now() >= *Params.Deadline)) return {};
		CleanupFinishedShaderMaps();
		const uint8 ReloadMode = GPendingShaderReloadMode.exchange(
			0, std::memory_order_acq_rel);
		if (ReloadMode != 0)
		{
			const std::vector<DObject*> Objects =
				GDObjectArray.Snapshot(EObjectQueryScope::LiveOnly);
			for (DObject* Object : Objects)
				if (auto* Material = Cast<DMaterialInterface>(Object); IsValid(Material))
				{
					const FMaterialRenderProxyRef Proxy =
						Material->GetMaterialRenderProxy();
					if (Proxy && Proxy->GetRefCount() > 2)
						RequestMaterialRecompile(*Material, ReloadMode > 1);
				}
		}

		FAssetCompileProcessResult Processed;
		FObjectCacheContext CompletionContext;
		Processed.ProcessedCompletionCount = ProcessMaterialResults(Params, [&](FMaterialCompileResult Result) {
			DObject* Object = Result.Owner.Get();
			if (auto* Material = Cast<DMaterialInterface>(Object); IsValid(Material)
				&& Private::FMaterialCompilationLifecycle::Admit(*Material, std::move(Result), &CompletionContext))
				Processed.SuccessfullyCompiledAssets.emplace_back(Material);
		});
		if (!Params.Deadline || std::chrono::steady_clock::now() < *Params.Deadline) Private::GetMaterialCompileRetryQueue().Process(
			Private::MaterialCompileMaxRetryChecks, [this, &Params](FWeakObjectPtr Owner) {
				auto* Material = Cast<DMaterialInterface>(Owner.Get());
				if (!IsValid(Material)) return false;
				const auto IsDeferred = [&] {
					const auto Status = Material->GetMaterialCompileStatus().State;
					return Status == EMaterialCompileState::Deferred
						|| Status == EMaterialCompileState::Scheduled;
				};
				if (!IsDeferred()) return false;
				if (Params.Deadline && std::chrono::steady_clock::now() >= *Params.Deadline) return true;
				if (!IsAcceptingMaterialRequests())
					Private::FMaterialCompilationLifecycle::MarkCanceled(*Material);
				else
					Private::FMaterialCompilationLifecycle::RetryDeferred(*Material);
				return IsDeferred();
			});
		return Processed;
	}
	auto FShaderCompilingManager::FinishAllCompilation() -> FAssetCompileProcessResult
	{
		FAssetCompileProcessResult Aggregate;
		for (;;) {
			RaiseMaterialPriority();
			FinishMaterialCompilation();
			FinishShaderJobs();
			auto Item = ProcessAsyncResults({.MaximumCompletions = std::numeric_limits<uint32>::max()});
			Aggregate.ProcessedCompletionCount += Item.ProcessedCompletionCount;
			Aggregate.SuccessfullyCompiledAssets.insert(Aggregate.SuccessfullyCompiledAssets.end(),
				Item.SuccessfullyCompiledAssets.begin(), Item.SuccessfullyCompiledAssets.end());
			if (GetNumRemainingAssets() == 0) break;
		}
		return Aggregate;
	}
	auto FShaderCompilingManager::Shutdown() -> void
	{
		StopAdmission();
		CancelAllCompilation();
		{
			std::lock_guard Lock(MaterialMutex);
			MaterialDiagnostics.CanceledRequests += OutstandingMaterialConsumers;
			RequestedShaderMaps.clear(); MaterialResults.clear(); OutstandingMaterialConsumers = 0;
			require(CapturedMaterialBytes == 0);
		}
		FMaterialShaderMap::ClearCache();
		ClearShaderMapPreparations();
	}
	auto FShaderCompilingManager::IsAcceptingMaterialRequests() const -> bool
	{
		return Accepting && IsTaskSchedulerRunning() && FAssetCompilingManager::Get().IsAcceptingRequests();
	}
}
