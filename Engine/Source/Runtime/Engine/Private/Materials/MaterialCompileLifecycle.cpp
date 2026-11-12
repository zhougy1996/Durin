#include "Materials/ObjectCacheContext.h"
#include "Materials/MaterialShaderMap.h"
#include "Shader/ShaderCompilingManager.h"
#include "MaterialCollectionBinding.h"
#include "MaterialPreparationQueue.h"
#include "Asset/OfflinePreparation.h"
#include "Asset/Asset.h"
#include "Asset/CookDependencies.h"
#include "DObject/Package.h"
#include "Materials/MaterialCompileLifecycle.h"

#include "Asset/AssetCompilingManager.h"
#include "CoreGlobals.h"
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

	}

	namespace Private
	{
		auto GetMaterialEditCompileQueue() -> FMaterialPreparationQueue&
		{
			CheckMaterialCompileGameThread();
			static FMaterialPreparationQueue Queue;
			return Queue;
		}

		auto FMaterialCompilationLifecycle::BeginRequest(DMaterialInterface& Material) -> void
		{
			CheckMaterialCompileGameThread();
			Material.CompilationState.EditCompileDeadline.reset();
			GetMaterialEditCompileQueue().Remove(FWeakObjectPtr(&Material));
			FShaderCompilingManager::Get().CancelCompilation(FWeakObjectPtr(&Material), false);
			auto& Status = Material.CompilationState.MaterialCompileStatus;
			Status.SubmittedRevision = Status.AuthoredRevision;
			Status.RequestId = AdvanceNonzero(Status.RequestId);
			Status.State = EMaterialCompileState::Running;
			Status.CacheOutcome = EMaterialCompileCacheOutcome::None;
			Material.CompilationState.MaterialCompileDiagnostics.clear();
			Material.CompilationState.RequestedExpressionSources.clear();
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
			GetMaterialEditCompileQueue().Remove(FWeakObjectPtr(&Material));
			Material.CompilationState.LastObservedShaderProperties =
				CanonicalizeMaterialShaderProperties(Material.GetStaticProperties());
			auto& Declarations = Material.CompilationState.LastObservedParameters;
			Declarations.clear();
			for (const auto& Definition : Material.GetParameterDefinitions())
				Declarations.push_back({Definition.Id, Definition.Type});
			std::ranges::sort(Declarations, {}, &FMaterialCompilerParameterDeclaration::Id);
			auto& Status = Material.CompilationState.MaterialCompileStatus;
			Status.CacheOutcome = EMaterialCompileCacheOutcome::None;
			Material.CompilationState.MaterialCompileDiagnostics.clear();
			Material.CompilationState.EditCompileDeadline.reset();
			if (Mode == EMaterialEditCompileMode::Automatic)
				Material.CompilationState.EditCompileDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
			if (Material.CompilationState.EditCompileDeadline)
				GetMaterialEditCompileQueue().Add(FWeakObjectPtr(&Material));
#endif
		}

		auto FMaterialCompilationLifecycle::Submit(
			DMaterialInterface& Material,
			MIR::FCompilerInput Input,
			bool bForceRecompile, FObjectCacheContext* Context) -> bool
		{
			CheckMaterialCompileGameThread();
			auto& Status = Material.CompilationState.MaterialCompileStatus;
			Status.DependencyRevision = GetShaderReloadGeneration();
			Status.Target = Input.Environment.Target;
			Material.CompilationState.RequestedExpressionSources = Input.Sources;
			FMaterialCompileRequest Request{
				.Owner = FWeakObjectPtr(&Material), .AuthoredRevision = Status.AuthoredRevision,
				.RequestId = Status.RequestId, .DependencyRevision = Status.DependencyRevision,
				.CompilerInput = std::make_shared<const MIR::FCompilerInput>(std::move(Input)),
				.bForceRecompile = bForceRecompile};
#if DURIN_WITH_EDITOR
			if (FShaderCompilingManager::Get().IsAcceptingMaterialRequests() && !FScopedOfflinePreparation::IsActive())
			{
				FShaderCompilingManager::Get().SubmitJobs(std::move(Request));
				return true;
			}
#endif
			auto Build = MaterialBuildPrivate::CompileMaterialInput(*Request.CompilerInput, bForceRecompile);
			Admit(Material, FShaderCompilingManager::MakeMaterialResult(Request, Build), Context);
			return Status.State == EMaterialCompileState::Ready;
		}

		auto FMaterialCompilationLifecycle::Admit(
			DMaterialInterface& Material, FMaterialCompileResult Result, FObjectCacheContext* Context) -> bool
		{
				CheckMaterialCompileGameThread();
				FMaterialCompileStatus& Status = Material.CompilationState.MaterialCompileStatus;
				if (Result.RequestId != Status.RequestId
					|| Result.AuthoredRevision != Status.AuthoredRevision
					|| Result.DependencyRevision != Status.DependencyRevision
					|| Result.Target != Status.Target
					|| Result.DependencyRevision != GetShaderReloadGeneration())
				{
					return false;
				}

				if (!SameOwner(Result.Owner, FWeakObjectPtr(&Material))) return false;

				if (!Result.ExpressionSources.empty()) Material.CompilationState.RequestedExpressionSources = std::move(Result.ExpressionSources);

				Status.State = Result.Outcome == EMaterialCompileOutcome::Succeeded ? EMaterialCompileState::Ready
					: Result.Outcome == EMaterialCompileOutcome::Canceled ? EMaterialCompileState::Canceled : EMaterialCompileState::Failed;
				Status.CacheOutcome = Result.CacheOutcome;
				Material.CompilationState.MaterialCompileDiagnostics = std::move(Result.Diagnostics);
				if (Result.Outcome == EMaterialCompileOutcome::Succeeded
					&& Result.CompiledProgram
					&& Result.CompiledProgram->GetShaderMap().ShaderMapId == Result.ProgramIdentity
					&& ValidateMaterialCompilerResult(*Result.CompiledProgram))
				{
					FMaterialResource Candidate;
					Candidate.CompiledProgram = Result.CompiledProgram;
					if (!Candidate.CompiledProgram->ActiveCollections.empty())
					{
						auto Current = MaterialBuildPrivate::ResolvePublicationCollections(Candidate.CompiledProgram->ActiveCollections);
						if (!Current) { Status.State = EMaterialCompileState::Rejected; Material.RetireFailedMaterialGeneration(Context); return false; }
						if (auto Refreshed = MaterialBuildPrivate::BindMaterialProgramCollections(Candidate.CompiledProgram, *Current)) Candidate.CompiledProgram = std::move(Refreshed);
					}
					const auto ShaderProperties = CanonicalizeMaterialShaderProperties(Result.StaticProperties);
					Candidate.StaticProperties = Material.GetStaticProperties();
					Candidate.StaticProperties->OpacityMaskThreshold = ShaderProperties.OpacityMaskThreshold;
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
					Material.CompilationState.RenderLayer = std::move(Candidate);
					Material.CompilationState.AcceptedExpressionSources = Material.CompilationState.RequestedExpressionSources;
					Status.CompiledIdentity = Result.ProgramIdentity;
					Status.CompiledRevision = Result.AuthoredRevision;
					Status.CompiledDependencyRevision = Result.DependencyRevision;
					Material.MarkRenderDataDirty(
						EMaterialRenderDirtyFlags::ShaderMap
							| EMaterialRenderDirtyFlags::PipelineState, false, Context);
					return true;
				}

				if (Result.Outcome == EMaterialCompileOutcome::Succeeded)
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
			Material.CancelFeatureLevelCompilation();
			Material.CompilationState.CookedPreparation.reset();
			GetMaterialEditCompileQueue().Remove(FWeakObjectPtr(&Material));
			GetMaterialPreparationQueue().Remove(FWeakObjectPtr(&Material));
			Material.CompilationState.EditCompileDeadline.reset();
			Material.CompilationState.MaterialCompileStatus.State = EMaterialCompileState::Canceled;
		}

		auto FMaterialCompilationLifecycle::SubmitScheduledEdit(DMaterialInterface& Material) -> void
		{
			if (!Material.CompilationState.EditCompileDeadline
				|| std::chrono::steady_clock::now() < *Material.CompilationState.EditCompileDeadline) return;
			RequestCurrent(Material, false);
		}

		auto FMaterialCompilationLifecycle::RequestCurrent(
			DMaterialInterface& Material, bool bForceRecompile, FObjectCacheContext* Context) -> bool
		{
			if (Material.IsDynamicInstance()) return false;
			if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) return false;
			FResolvedMaterialProperties Resolved;
			const auto Error = ResolveMaterialProperties(Material, Resolved);
			if (!Error)
			{
				BeginRequest(Material);
				Material.CompilationState.MaterialCompileStatus.State = EMaterialCompileState::Failed;
				Material.CompilationState.MaterialCompileDiagnostics = {{
					.Message = Error.error()}};
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
		return FShaderCompilingManager::Get().IsMaterialCompiling(FWeakObjectPtr(const_cast<DMaterialInterface*>(&Material)))
			|| (GetAssetRuntimeConfiguration().RequiresCookedPayload()
				&& Material.GetMaterialCompileStatus().State == EMaterialCompileState::Running);
	}

	auto GetMaterialCompilationDiagnostics()
		-> FMaterialCompilationDiagnostics
	{
		return FShaderCompilingManager::Get().GetMaterialDiagnostics();
	}

	auto RequestMaterialRecompile(
		DMaterialInterface& Material, bool bForceRecompile) -> bool
	{
		return Private::FMaterialCompilationLifecycle::RequestCurrent(
			Material, bForceRecompile);
	}

}
