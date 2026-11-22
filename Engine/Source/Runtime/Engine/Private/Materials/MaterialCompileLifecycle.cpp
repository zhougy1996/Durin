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
			const std::shared_ptr<FMaterialResource>& Resource) -> void
		{
			CheckMaterialCompileGameThread();
			FShaderCompilingManager::Get().CancelCompilation(Resource);
			Resource->CookedPreparation.reset();
			Resource->RequestedInput = Material.CaptureCompilationInputStamp();
			Resource->RequestId = Material.CompilationState.NextRequestId = AdvanceNonzero(Material.CompilationState.NextRequestId);
			Resource->Outcome = EMaterialCompileState::NeverRequested;
			Resource->CacheInfo = {};
			Resource->CookDiagnostic.clear();
			Resource->Diagnostics.clear();
			Resource->RequestedExpressionSources.clear();
			if (Resource == Material.CompilationState.SelectedResource)
			{
				Material.CompilationState.SubmittedRevision = Material.CompilationState.AuthoredRevision;
				Material.CompilationState.EditCompileDeadline.reset();
				GetMaterialEditCompileQueue().Remove(FWeakObjectPtr(&Material));
			}
		}

		auto FMaterialCompilationLifecycle::Invalidate(DMaterialInterface& Material) -> void
		{
			const auto Detach = [&](const auto& Resource) {
				if (!Resource || Resource->RequestedInput.ContentRevision == Material.CompilationState.AuthoredRevision) return;
				FShaderCompilingManager::Get().CancelCompilation(Resource);
				Resource->CookedPreparation.reset();
				Resource->ClearAcceptedProgram();
			};
			for (const auto& Resource : Material.CompilationState.Resources) Detach(Resource);
		}

		auto FMaterialCompilationLifecycle::ScheduleEdit(DMaterialInterface& Material, FObjectCacheContext* Context) -> void
		{
			CheckMaterialCompileGameThread();
#if DURIN_WITH_EDITORONLY_DATA
			if (Material.IsDynamicInstance()) return;
			Invalidate(Material);
			Material.PublishMaterialRenderProxyState(EMaterialRenderDirtyFlags::ShaderMap | EMaterialRenderDirtyFlags::PipelineState);
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
			auto& Status = *Material.CompilationState.SelectedResource;
			Status.CacheInfo = {};
			Material.CompilationState.SelectedResource->Diagnostics.clear();
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
			std::shared_ptr<FMaterialResource> Resource) -> bool
		{
			CheckMaterialCompileGameThread();
			if (!Resource)
			{
				Resource = Material.GetCompilationResource(Input.Environment.FeatureLevel, Input.Environment.Quality, true);
				Material.CompilationState.SelectedResource = Resource;
			}
			ResetRequest(Material, Resource);

			Resource->RequestedExpressionSources = Input.Sources;
			Resource->Material = FWeakObjectPtr(&Material);
			Resource->bRequested = true;
			auto Request = std::make_shared<FMaterialCompileRequest>(FMaterialCompileRequest{
				.Resource = Resource,
				.CompilerInput = std::make_shared<const MIR::FCompilerInput>(std::move(Input)),
				.bForceRecompile = bForceRecompile});
			Resource->Compilation = Request;
			GetMaterialPreparationQueue().Add(FWeakObjectPtr(&Material));
#if DURIN_WITH_EDITOR
			if (FShaderCompilingManager::Get().IsAcceptingMaterialRequests() && !FScopedOfflinePreparation::IsActive())
			{
				FShaderCompilingManager::Get().CacheMaterialShaders(std::move(Request));
				return true;
			}
			MaterialBuildPrivate::FMaterialBuildResult Build;
			auto& Manager = FShaderCompilingManager::Get();
			const auto Handle = Manager.Submit(Request->CompilerInput, {.bForceRecompile = bForceRecompile},
				[&](auto Result) { Build = std::move(Result); });
			if (Handle) Manager.FinishCompilation(Handle);
#else
			auto Build = MaterialBuildPrivate::CompileMaterialInput(*Request->CompilerInput, bForceRecompile);
#endif
			Admit(Material, FShaderCompilingManager::MakeMaterialResult(Request, Build), Context);
			return Resource->Outcome == EMaterialCompileState::Ready;
		}

		auto FMaterialCompilationLifecycle::Admit(
			DMaterialInterface& Material, FMaterialCompileResult Result, FObjectCacheContext* Context) -> bool
		{
			CheckMaterialCompileGameThread();
			const auto Resource = Result.Request->Resource;
			if (Resource->Compilation.lock() != Result.Request) return false;
			Resource->Compilation.reset();
			if (Resource->RequestedInput != Material.CaptureCompilationInputStamp()) return false;
			auto& Status = *Resource;
			Resource->bRequested = false;

			const auto Retire = [&] {
				Resource->bRequested = false;
				Resource->ClearAcceptedProgram();
				Resource->AcceptedExpressionSources.clear();
				Material.MarkRenderDataDirty(EMaterialRenderDirtyFlags::ShaderMap | EMaterialRenderDirtyFlags::PipelineState, false, Context);
			};

			if (!Result.ExpressionSources.empty()) Resource->RequestedExpressionSources = std::move(Result.ExpressionSources);

			Status.Outcome = Result.Outcome == EMaterialProgramStatus::Ok ? EMaterialCompileState::Ready
				: Result.Outcome == EMaterialProgramStatus::Canceled ? EMaterialCompileState::Canceled : EMaterialCompileState::Failed;
			if (Status.Outcome == EMaterialCompileState::Canceled) Resource->bRequested = false;
			Status.CacheInfo = Result.CacheInfo;
			Resource->Diagnostics = std::move(Result.Diagnostics);
			if (Result.Outcome == EMaterialProgramStatus::Ok)
			{
				if (!Result.Request->CompilerInput)
				{
					const auto Installed = Material.AdmitCookedProgram(Result.StaticProperties, Result.CookedProgram, Resource);
					if (Installed) return true;
					Resource->Diagnostics.push_back({.Message = Installed.error()});
					Resource->CookDiagnostic = Installed.error();
					Resource->Outcome = EMaterialCompileState::Failed;
					Retire();
					return false;
				}
				auto Bound = std::make_shared<FMaterialProgram>();
				if (!MaterialBuildPrivate::BindMaterialShaderMap(*Bound, std::move(Result.CompiledShaderMap),
					*Result.Request->CompilerInput, MaterialBuildPrivate::EMaterialCollectionSource::LiveObjects, Resource->Diagnostics)) {
					Status.Outcome = EMaterialCompileState::Failed;
					Retire();
					return false;
				}
				const auto ShaderProperties = CanonicalizeMaterialShaderProperties(Result.StaticProperties);
				auto Properties = Material.GetStaticProperties();
				Properties.OpacityMaskThreshold = ShaderProperties.OpacityMaskThreshold;
				auto Program = Material.BindMaterialDefaultParameters(std::move(Bound));
				if (!Program) Resource->Diagnostics.push_back({.Message = "Material default parameters no longer match the compiled shader map."});
				else if (const auto Validation = ValidateMaterialProgram(*Program); !Validation) {
					Resource->Diagnostics.push_back({.Message = std::format("{} [index {}]", GetMaterialLayoutErrorText(Validation.Error), Validation.FieldIndex)});
					Program.reset();
				}
				if (!Program) { Status.Outcome = EMaterialCompileState::Failed; Retire(); return false; }
				Resource->CompiledProgram = std::move(Program);
				Resource->AcceptedProperties = std::move(Properties);
				Resource->AcceptedExpressionSources = Resource->RequestedExpressionSources;
				Status.CompiledRevision = Resource->RequestedInput.ContentRevision;
				Status.CompiledDependencyRevision = Resource->RequestedInput.ShaderRevision;
				Material.MarkRenderDataDirty(
					EMaterialRenderDirtyFlags::ShaderMap
						| EMaterialRenderDirtyFlags::PipelineState, false, Context);
				return true;
			}

			if (Status.Outcome == EMaterialCompileState::Failed)
				Retire();
			return false;
		}

		auto FMaterialCompilationLifecycle::MarkCanceled(
			DMaterialInterface& Material) -> void
		{
			FShaderCompilingManager::Get().CancelCompilation(FWeakObjectPtr(&Material));
			Material.CancelResourceCompilation();
			Material.CompilationState.SelectedResource->CookedPreparation.reset();
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
				ResetRequest(Material, Material.CompilationState.SelectedResource);
				Material.CompilationState.SelectedResource->Outcome = EMaterialCompileState::Failed;
				Material.CompilationState.SelectedResource->Diagnostics = {{
					.Message = Error.error()}};
				Material.RetireFailedMaterialGeneration(Context);
				return false;
			}
			return Material.RequestProgramCompile(bForceRecompile, Context);
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
			return Resource && !Resource->Compilation.expired();
		};
		return std::ranges::any_of(Material.CompilationState.Resources, Pending);
	}

	auto GetMaterialCompilationStatistics()
		-> FMaterialCompilationStatistics
	{
		return FShaderCompilingManager::Get().GetMaterialStatistics();
	}

	auto RequestMaterialRecompile(
		DMaterialInterface& Material, bool bForceRecompile) -> bool
	{
		return Private::FMaterialCompilationLifecycle::RequestCurrent(
			Material, bForceRecompile);
	}

}
