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
#include "DynamicRHI.h"
#include "RHIGlobals.h"
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

	}

	namespace Private
	{
		auto GetMaterialEditCompileQueue() -> FMaterialPreparationQueue&
		{
			CheckMaterialCompileGameThread();
			static FMaterialPreparationQueue Queue;
			return Queue;
		}

		auto FMaterialCompilationLifecycle::ResetRequest(DMaterialInterface& Material,
			const std::shared_ptr<FMaterialCompilationResource>& Resource) -> void
		{
			CheckMaterialCompileGameThread();
			FShaderCompilingManager::Get().CancelCompilation(Resource);
			Resource->CookedPreparation.reset();
			auto& Status = Resource->Status;
			Status.SubmittedRevision = Status.AuthoredRevision;
			Status.RequestId = AdvanceNonzero(Status.RequestId);
			Status.State = EMaterialCompileState::NeverRequested;
			Status.CacheOutcome = EMaterialCompileCacheOutcome::None;
			Resource->Diagnostics.clear();
			Resource->RequestedExpressionSources.clear();
			if (Resource == Material.CompilationState.DefaultResource)
			{
				Material.CompilationState.EditCompileDeadline.reset();
				GetMaterialEditCompileQueue().Remove(FWeakObjectPtr(&Material));
			}
		}

		auto FMaterialCompilationLifecycle::Invalidate(DMaterialInterface& Material) -> void
		{
			const auto Detach = [&](const auto& Resource) {
				if (!Resource || Resource->Status.SubmittedRevision == Material.CompilationState.DefaultResource->Status.AuthoredRevision) return;
				FShaderCompilingManager::Get().CancelCompilation(Resource);
				Resource->CookedPreparation.reset();
			};
			Detach(Material.CompilationState.DefaultResource);
			for (const auto& Resource : Material.FeatureLevelResources) Detach(Resource);
		}

		auto FMaterialCompilationLifecycle::ScheduleEdit(DMaterialInterface& Material, FObjectCacheContext* Context) -> void
		{
			CheckMaterialCompileGameThread();
#if DURIN_WITH_EDITORONLY_DATA
			if (Material.IsDynamicInstance()) return;
			Invalidate(Material);
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
			auto& Status = Material.CompilationState.DefaultResource->Status;
			Status.CacheOutcome = EMaterialCompileCacheOutcome::None;
			Material.CompilationState.DefaultResource->Diagnostics.clear();
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
			bool bForceRecompile, FObjectCacheContext* Context,
			std::shared_ptr<FMaterialCompilationResource> Resource) -> bool
		{
			CheckMaterialCompileGameThread();
			if (!Resource) Resource = Material.CompilationState.DefaultResource;
			ResetRequest(Material, Resource);
			auto& Status = Resource->Status;
			Resource->ShaderRevision = GetShaderReloadGeneration();
			Resource->FeatureLevel = Input.Environment.FeatureLevel;
			Resource->Quality = Input.Environment.Quality;
			Resource->Backend = GetActiveRHIBackend().value_or(ResolveRHIBackend(std::getenv("DURIN_RHI_BACKEND")).value_or(ERHIBackend::Vulkan));
			Resource->BulkRevision = Material.CookedProgramData.GetContentRevision();
			Resource->bCooked = false;
			Resource->CookRoot = GetAssetRuntimeConfiguration().GetCookRoot();
			Resource->CookTarget = GetAssetRuntimeConfiguration().GetCookTargetPlatform();
			Status.Target = Input.Environment.Target;
			Resource->RequestedExpressionSources = Input.Sources;
			Resource->Material = FWeakObjectPtr(&Material);
			Resource->bPending = true;
			FMaterialCompileRequest Request{
				.Resource = Resource,
				.RequestId = Status.RequestId, .ShaderRevision = Resource->ShaderRevision,
				.CompilerInput = std::make_shared<const MIR::FCompilerInput>(std::move(Input)),
				.bForceRecompile = bForceRecompile};
#if DURIN_WITH_EDITOR
			if (FShaderCompilingManager::Get().IsAcceptingMaterialRequests() && !FScopedOfflinePreparation::IsActive())
			{
				FShaderCompilingManager::Get().CacheMaterialShaders(std::move(Request));
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
			const auto Resource = Result.Resource;
			if (!Resource || !Resource->bPending || Result.RequestId != Resource->Status.RequestId) return false;
			Resource->bPending = false;
			auto& Status = Resource->Status;
			if (Result.ShaderRevision != GetShaderReloadGeneration()) return false;

			const auto Retire = [&] {
				Resource->bRequested = false;
				Resource->RenderLayer = {};
				Resource->AcceptedExpressionSources.clear();
				Material.MarkRenderDataDirty(EMaterialRenderDirtyFlags::ShaderMap | EMaterialRenderDirtyFlags::PipelineState, false, Context);
			};

			if (!Result.ExpressionSources.empty()) Resource->RequestedExpressionSources = std::move(Result.ExpressionSources);

			Status.State = Result.Outcome == EMaterialCompileOutcome::Succeeded ? EMaterialCompileState::Ready
				: Result.Outcome == EMaterialCompileOutcome::Canceled ? EMaterialCompileState::Canceled : EMaterialCompileState::Failed;
			if (Status.State == EMaterialCompileState::Canceled) Resource->bRequested = false;
			Status.CacheOutcome = Result.CacheOutcome;
			Resource->Diagnostics = std::move(Result.Diagnostics);
			if (Result.Outcome == EMaterialCompileOutcome::Succeeded
				&& Result.CompiledProgram
				&& ValidateMaterialCompilerResult(*Result.CompiledProgram))
			{
				FMaterialResource Candidate;
				Candidate.CompiledProgram = Result.CompiledProgram;
				if (!Candidate.CompiledProgram->ActiveCollections.empty())
				{
					auto Current = MaterialBuildPrivate::ResolvePublicationCollections(Candidate.CompiledProgram->ActiveCollections);
					if (!Current) { Status.State = EMaterialCompileState::Failed; Retire(); return false; }
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
						Status.State = EMaterialCompileState::Failed;
						Retire();
						return false;
					}
					Candidate.Parameters.push_back(BuildMaterialLocalRenderParameter(
						Parameter.Id, Resolved.Value));
				}
				Resource->RenderLayer = std::move(Candidate);
				Resource->AcceptedExpressionSources = Resource->RequestedExpressionSources;
				Status.CompiledRevision = Status.AuthoredRevision;
				Status.CompiledDependencyRevision = Result.ShaderRevision;
				Material.MarkRenderDataDirty(
					EMaterialRenderDirtyFlags::ShaderMap
						| EMaterialRenderDirtyFlags::PipelineState, false, Context);
				return true;
			}

			if (Result.Outcome == EMaterialCompileOutcome::Succeeded)
			{
				Status.State = EMaterialCompileState::Failed;
			}
			if (Status.State == EMaterialCompileState::Failed)
				Retire();
			return false;
		}

		auto FMaterialCompilationLifecycle::MarkCanceled(
			DMaterialInterface& Material) -> void
		{
			Material.CancelFeatureLevelCompilation();
			Material.CompilationState.DefaultResource->CookedPreparation.reset();
			GetMaterialEditCompileQueue().Remove(FWeakObjectPtr(&Material));
			GetMaterialPreparationQueue().Remove(FWeakObjectPtr(&Material));
			Material.CompilationState.EditCompileDeadline.reset();
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
				ResetRequest(Material, Material.CompilationState.DefaultResource);
				Material.CompilationState.DefaultResource->Status.State = EMaterialCompileState::Failed;
				Material.CompilationState.DefaultResource->Diagnostics = {{
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
		const auto Pending = [](const auto& Resource) {
			return Resource && (Resource->bPending || Resource->CookedPreparation);
		};
		return Pending(Material.CompilationState.DefaultResource)
			|| std::ranges::any_of(Material.FeatureLevelResources, Pending);
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
