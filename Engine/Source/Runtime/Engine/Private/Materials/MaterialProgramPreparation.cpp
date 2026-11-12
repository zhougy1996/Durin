#include "Shader/ShaderCompilerCore.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialCookedProgram.h"
#include "MaterialPreparationQueue.h"
#include "Asset/Asset.h"
#include "DObject/WeakObjectPtr.h"
#include "DynamicRHI.h"
#include "RHIGlobals.h"
#include "Threading/TaskRuntime.h"
#include "Threading/RunnableThread.h"
#include "Shader/ShaderCompilingManager.h"
#include "Materials/MaterialCompilationConfiguration.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialCompileTypes.h"
#include "Materials/MaterialShaderMap.h"
#include "RenderingThread.h"

namespace Durin
{
	struct FMaterialProgramDecodeResult
	{
		std::atomic_bool Canceled = false;
		FMaterialStaticProperties Properties;
		std::shared_ptr<const FMaterialCompilerResult> Program;
		std::string Diagnostic;
	};
	// Only request identity and detached work live here. The material owns compile
	// status, diagnostics and the accepted render program.
	struct FMaterialProgramPreparationState
	{
		uint64 BulkRevision = 0;
		std::filesystem::path CookRoot;
		ECookTargetPlatform CookTarget = ECookTargetPlatform::Win64;
		EMaterialQualityLevel Quality;
		ERHIFeatureLevel FeatureLevel;
		ERHIBackend Backend;
		std::optional<FPackageResourceRequest> Request;
		std::shared_ptr<FMaterialProgramDecodeResult> Result = std::make_shared<FMaterialProgramDecodeResult>();
		~FMaterialProgramPreparationState() { Result->Canceled = true; }
	};
	static auto PreparationBackend() -> ERHIBackend
	{
		return GetActiveRHIBackend().value_or(ResolveRHIBackend(std::getenv("DURIN_RHI_BACKEND")).value_or(ERHIBackend::Vulkan));
	}
	static auto PreparationFeatureLevel() -> ERHIFeatureLevel
	{
		return GDynamicRHI && GDynamicRHI->RHIGetCapabilities() ? GDynamicRHI->RHIGetCapabilities()->FeatureLevel : ERHIFeatureLevel::SM5;
	}
	struct FPreparedMaterialProgram
	{
		FMaterialStaticProperties Properties;
		std::shared_ptr<const FMaterialCompilerResult> Program;
	};
	struct FMaterialPreparationError
	{
		EMaterialCompileState State;
		std::string Diagnostic;
	};
	static auto CreateProgramPreparation(const DMaterialInterface& Material, ERHIFeatureLevel Level)
		-> std::shared_ptr<FMaterialProgramPreparationState>
	{
		auto State = std::make_shared<FMaterialProgramPreparationState>();
		State->BulkRevision = Material.GetCookedProgramData().GetContentRevision();
		State->CookRoot = GetAssetRuntimeConfiguration().GetCookRoot();
		State->CookTarget = GetAssetRuntimeConfiguration().GetCookTargetPlatform();
		State->Quality = GetMaterialQualityLevel();
		State->FeatureLevel = Level;
		State->Backend = PreparationBackend();
		return State;
	}
	static auto PollProgramPreparation(const DMaterialInterface& Material,
		const std::shared_ptr<FMaterialProgramPreparationState>& State, bool bSynchronous)
		-> std::expected<std::optional<FPreparedMaterialProgram>, FMaterialPreparationError>
	{
		const auto Finish = [](EMaterialCompileState Status, std::string Diagnostic)
			-> std::expected<std::optional<FPreparedMaterialProgram>, FMaterialPreparationError> {
			return std::unexpected(FMaterialPreparationError{Status, std::move(Diagnostic)});
		};
		const auto Metadata = Material.GetCookedProgramData().GetMetadata();
		if (!GetAssetRuntimeConfiguration().RequiresCookedPayload()
			|| GetAssetRuntimeConfiguration().GetCookRoot() != State->CookRoot
			|| GetAssetRuntimeConfiguration().GetCookTargetPlatform() != State->CookTarget
			|| (Metadata.Range.Resource && Metadata.Range.Resource->IsRetired())
			|| Material.GetCookedProgramData().GetContentRevision() != State->BulkRevision
			|| State->Quality != GetMaterialQualityLevel()
			|| State->FeatureLevel > PreparationFeatureLevel() || State->Backend != PreparationBackend())
			return Finish(EMaterialCompileState::Canceled, "Cooked program request was invalidated before publication.");
		if (!State->Request)
		{
			if (Metadata.LogicalSize == 0 || Metadata.LogicalSize > MaterialCookedProgramMaxPayloadBytes)
				return Finish(EMaterialCompileState::Failed, "Cooked program preparation requires bounded cooked data.");
			const auto Decode = [Result = State->Result, Quality = State->Quality,
				Level = State->FeatureLevel, Backend = State->Backend](FByteView Bytes) {
				const auto Decoded = DecodeMaterialCookedProgramFamily(Bytes,
					Backend == ERHIBackend::Metal ? ECookTargetPlatform::MacOS : ECookTargetPlatform::Win64,
					ECookTargetProfile::Game, Quality, Level, {}, Result->Properties, Result->Program);
				if (!Decoded) Result->Diagnostic = Decoded.error();
			};
			if (bSynchronous)
			{
				FBulkData Snapshot = Material.GetCookedProgramData();
				auto Read = Snapshot.AcquireRead();
				if (!Read) return Finish(EMaterialCompileState::Failed, Read.error().Message);
				Decode(Read->GetBytes());
			}
			else
			{
				if (!IsTaskSchedulerRunning())
					return Finish(EMaterialCompileState::Failed, "Cooked program task scheduler stopped before admission.");

				FBulkData Snapshot = Material.GetCookedProgramData();
				State->Request = FPackageResourceRequest::Transform(Snapshot.ReloadAsync(),
					[Result = State->Result, Decode](FPackageResourceReadResult Read) {
						if (Result->Canceled) return Read;
						if (!Read) { Result->Diagnostic = FormatPackageResourceReadError(Read); return Read; }
						Decode(Read->GetBytes());
						return Read;
					});
			}
		}
		if (State->Request)
		{
			if (!State->Request->IsReady() && !bSynchronous && IsTaskSchedulerRunning()) return std::optional<FPreparedMaterialProgram>{};
			// Join the existing read/decode even in synchronous mode; never submit it twice.
			const auto Read = State->Request->Wait();
			if (!Read) return Finish(EMaterialCompileState::Failed, FormatPackageResourceReadError(Read));
		}
		if (!State->Result->Program || !State->Result->Diagnostic.empty())
		{
			return Finish(EMaterialCompileState::Failed, State->Result->Diagnostic);
		}
		return FPreparedMaterialProgram{State->Result->Properties, State->Result->Program};
	}
	auto DMaterialInterface::GetCompilationResource(ERHIFeatureLevel Level, bool Create) const -> std::shared_ptr<FMaterialCompilationResource>
	{
		if (CompilationState.DefaultResource->FeatureLevel == Level
			|| (!CompilationState.DefaultResource->RenderLayer.CompiledProgram && !CompilationState.DefaultResource->bPending && Level == PreparationFeatureLevel()))
			return CompilationState.DefaultResource;
		auto& Resource = FeatureLevelResources[static_cast<size_t>(Level)];
		if (!Resource && Create) Resource = std::make_shared<FMaterialCompilationResource>();
		return Resource;
	}

	auto DMaterialInterface::CacheResourceShadersForRendering(EMaterialShaderPrecompileMode Mode) -> void
	{
		CheckGameThread();
		if (Mode == EMaterialShaderPrecompileMode::None || IsPendingKill()) return;
		if (IsDynamicInstance()) { if (IsValid(GetParent())) GetParent()->CacheResourceShadersForRendering(Mode); return; }
		if (!GetAssetRuntimeConfiguration().RequiresCookedPayload()
			&& (IsComplete() || CompilationState.DefaultResource->bPending))
		{
			if (Mode == EMaterialShaderPrecompileMode::Synchronous && CompilationState.DefaultResource->bPending)
			{
				const std::array<DObject*, 1> Objects{this};
				FShaderCompilingManager::Get().FinishCompilationForObjects(Objects);
			}
			return;
		}
		CacheResourceShaders(CompilationState.DefaultResource, PreparationFeatureLevel(), Mode);
	}

	auto DMaterialInterface::CacheResourceShadersForRendering(ERHIFeatureLevel Level, EMaterialShaderPrecompileMode Mode) -> void
	{
		CheckGameThread();
		if (Mode == EMaterialShaderPrecompileMode::None || IsPendingKill() || !IsSupportedMaterialFeatureLevel(Level)) return;
		if (IsDynamicInstance()) { if (IsValid(GetParent())) GetParent()->CacheResourceShadersForRendering(Level, Mode); return; }
		CacheResourceShaders(GetCompilationResource(Level, true), Level, Mode);
	}

	auto DMaterialInterface::CacheResourceShaders(const std::shared_ptr<FMaterialCompilationResource>& Resource,
		ERHIFeatureLevel Level, EMaterialShaderPrecompileMode Mode) -> void
	{
		const bool Synchronous = Mode == EMaterialShaderPrecompileMode::Synchronous;
		const bool Cooked = GetAssetRuntimeConfiguration().RequiresCookedPayload();
		const auto& OwnerStatus = CompilationState.DefaultResource->Status;
		Resource->bRequested = true;
		const bool SameInput = Resource->FeatureLevel == Level
			&& Resource->Status.SubmittedRevision == OwnerStatus.AuthoredRevision
			&& Resource->ShaderRevision == GetShaderReloadGeneration() && Resource->Quality == GetMaterialQualityLevel()
			&& Resource->BulkRevision == CookedProgramData.GetContentRevision() && Resource->Backend == PreparationBackend()
			&& Resource->bCooked == Cooked && Resource->CookRoot == GetAssetRuntimeConfiguration().GetCookRoot()
			&& Resource->CookTarget == GetAssetRuntimeConfiguration().GetCookTargetPlatform();
		if (!SameInput)
		{
			FShaderCompilingManager::Get().CancelCompilation(Resource);
			Resource->CookedPreparation.reset();
			Resource->Status.State = EMaterialCompileState::NeverRequested;
			Resource->FeatureLevel = Level;
			Resource->Status.AuthoredRevision = OwnerStatus.AuthoredRevision;
			Resource->Status.SubmittedRevision = OwnerStatus.AuthoredRevision;
			Resource->ShaderRevision = GetShaderReloadGeneration();
			Resource->Quality = GetMaterialQualityLevel();
			Resource->BulkRevision = CookedProgramData.GetContentRevision();
			Resource->Backend = PreparationBackend();
			Resource->bCooked = Cooked;
			Resource->CookRoot = GetAssetRuntimeConfiguration().GetCookRoot();
			Resource->CookTarget = GetAssetRuntimeConfiguration().GetCookTargetPlatform();
		}
		const auto Fail = [&](std::string Message) {
			Resource->Status.State = EMaterialCompileState::Failed;
			Resource->CookDiagnostic = std::move(Message);
			Resource->bRequested = false;
			Resource->RenderLayer = {};
			Resource->AcceptedExpressionSources.clear();
			Resource->CookedPreparation.reset();
			MarkRenderDataDirty(EMaterialRenderDirtyFlags::ShaderMap | EMaterialRenderDirtyFlags::PipelineState);
		};
		if (Level > PreparationFeatureLevel()) { Fail("Material feature level exceeds RHI capability."); return; }
		if (Resource->bPending)
		{
			if (Synchronous)
			{
				const std::array<DObject*, 1> Objects{this};
				FShaderCompilingManager::Get().FinishCompilationForObjects(Objects);
			}
			return;
		}
		if (SameInput && !Resource->CookedPreparation && Resource->Status.State == EMaterialCompileState::Ready) return;
		if (!Cooked)
		{
			FMaterialCompilerEnvironment Environment;
			const auto Built = BuildDefaultMaterialCompilerEnvironment(Environment);
			if (!Built) { Fail(Built.error()); return; }
			Environment.FeatureLevel = Level;
			RequestProgramCompile(GetStaticProperties(), false, nullptr, std::move(Environment), Resource);
			if (Synchronous && Resource->bPending)
			{
				const std::array<DObject*, 1> Objects{this};
				FShaderCompilingManager::Get().FinishCompilationForObjects(Objects);
			}
			return;
		}
		if (!Resource->CookedPreparation)
		{
			if (!Synchronous && !IsTaskSchedulerRunning()) { Fail("Background cooked program preparation requires a running task scheduler."); return; }
			Resource->CookDiagnostic.clear();
			Resource->CookedPreparation = CreateProgramPreparation(*this, Level);
		}
		const auto Prepared = PollProgramPreparation(*this, Resource->CookedPreparation, Synchronous);
		if (!Prepared) { Fail(Prepared.error().Diagnostic); return; }
		if (!*Prepared) { Private::GetMaterialPreparationQueue().Add(FWeakObjectPtr(this)); return; }
		const auto Installed = AdmitCookedProgram((*Prepared)->Properties, (*Prepared)->Program, Resource);
		if (!Installed) Fail(Installed.error());
		else Resource->CookDiagnostic.clear();
	}

	auto DMaterialInterface::PollCookedProgramPreparation(bool Synchronous) -> bool
	{
		if (!CompilationState.DefaultResource->CookedPreparation) return false;
		CacheResourceShaders(CompilationState.DefaultResource, PreparationFeatureLevel(), Synchronous
			? EMaterialShaderPrecompileMode::Synchronous : EMaterialShaderPrecompileMode::Background);
		return CompilationState.DefaultResource->CookedPreparation != nullptr;
	}

	auto DMaterialInterface::GetAcceptedCompiledProgram(ERHIFeatureLevel Level) const -> std::shared_ptr<const FMaterialCompilerResult>
	{
		if (!IsSupportedMaterialFeatureLevel(Level)) return {};
		if (IsDynamicInstance()) return IsValid(GetParent()) ? GetParent()->GetAcceptedCompiledProgram(Level) : nullptr;
		const auto Resource = GetCompilationResource(Level);
		const auto Program = Resource ? Resource->RenderLayer.CompiledProgram : nullptr;
		return Program && Program->FeatureLevel == Level ? Program : nullptr;
	}

	auto DMaterialInterface::GetMaterialResource(ERHIFeatureLevel Level) const -> FMaterialResource
	{
		if (!IsSupportedMaterialFeatureLevel(Level)) return {};
		if (IsDynamicInstance())
		{
			if (!IsValid(GetParent())) return {};
			auto Resource = GetParent()->GetMaterialResource(Level);
			const auto* Instance = Cast<DMaterialInstance>(this);
			for (auto& Parameter : Resource.Parameters)
			{
				FMaterialParameterValue Value;
				if (Instance->GetLocalParameterValue(Parameter.Id, Value) && Value.GetType() == Parameter.GetType())
					Parameter = BuildMaterialLocalRenderParameter(Parameter.Id, Value);
			}
			return Resource;
		}
		const auto State = GetCompilationResource(Level);
		if (!State) return {};
		auto Resource = State->RenderLayer;
		if (Resource.CompiledProgram && Resource.CompiledProgram->FeatureLevel != Level) Resource = {};
		const auto& Authored = GetStaticProperties();
		if (Resource.StaticProperties && CanonicalizeMaterialShaderProperties(*Resource.StaticProperties) == CanonicalizeMaterialShaderProperties(Authored))
		{
			Resource.StaticProperties->bTwoSided = Authored.bTwoSided;
			Resource.StaticProperties->DepthWritePolicy = Authored.DepthWritePolicy;
		}
		Resource.bIsCompiling = State->bPending || State->CookedPreparation;
		return BuildMaterialResource(std::move(Resource));
	}

	auto DMaterialInterface::GetMaterialCompileStatus(ERHIFeatureLevel Level) const -> FMaterialCompileStatus
	{
		if (!IsSupportedMaterialFeatureLevel(Level)) return {.State = EMaterialCompileState::Failed};
		if (IsDynamicInstance()) return IsValid(GetParent()) ? GetParent()->GetMaterialCompileStatus(Level) : FMaterialCompileStatus{};
		const auto Resource = GetCompilationResource(Level);
		if (Resource == CompilationState.DefaultResource) return GetMaterialCompileStatus();
		if (!Resource) return {};
		auto Status = Resource->Status;
		Status.AuthoredRevision = CompilationState.DefaultResource->Status.AuthoredRevision;
		Status.State = Resource->bPending || Resource->CookedPreparation ? EMaterialCompileState::Running : Status.State;
		Status.CompiledIdentity = Resource->RenderLayer.CompiledProgram ? Resource->RenderLayer.CompiledProgram->GetShaderMap().ShaderMapId : FMaterialProgramIdentity{};
		Status.bDependenciesCurrent = Status.CompiledDependencyRevision == GetShaderReloadGeneration()
			&& Resource->RenderLayer.CompiledProgram && Resource->RenderLayer.CompiledProgram->FeatureLevel == Level
			&& Resource->RenderLayer.CompiledProgram->Quality == Resource->Quality;
		return Status;
	}

	auto DMaterialInterface::GetMaterialCookDiagnostic(ERHIFeatureLevel Level) const -> std::string
	{
		if (!IsSupportedMaterialFeatureLevel(Level)) return "Unsupported material feature level.";
		if (IsDynamicInstance()) return IsValid(GetParent()) ? GetParent()->GetMaterialCookDiagnostic(Level) : "Missing material parent.";
		const auto Resource = GetCompilationResource(Level);
		if (!Resource) return {};
		if (!Resource->CookDiagnostic.empty()) return Resource->CookDiagnostic;
		return Resource->Diagnostics.empty() ? std::string{} : FormatMaterialProgramDiagnostics(Resource->Diagnostics, "Material compilation failed.");
	}

	auto DMaterialInterface::GetRenderProxy(ERHIFeatureLevel Level) const -> FMaterialRenderProxyRef
	{
		CheckGameThread();
		if (IsTemplateObject() || !bAcceptingMaterialProxyPublications || !IsSupportedMaterialFeatureLevel(Level)) return {};
		const auto State = GetCompilationResource(Level, true);
		if (State == CompilationState.DefaultResource) return GetRenderProxy();
		if (!State->Proxy) State->Proxy = MakeRefCount<FMaterialRenderProxy>();
		if (GetRenderCommandAdmissionState() == ERenderCommandAdmissionState::Running)
		{
			auto Resource = GetMaterialResource(Level);
			std::ranges::sort(Resource.Parameters, {}, &FMaterialLocalRenderParameter::Id);
			State->Proxy->QueuePublication_GameThread(std::move(Resource));
		}
		return State->Proxy;
	}

	auto DMaterialInterface::PublishFeatureLevelResources() const -> void
	{
		if (!bAcceptingMaterialProxyPublications) return;
		for (size_t Index = 0; Index < FeatureLevelResources.size(); ++Index)
		{
			const auto& Resource = FeatureLevelResources[Index];
			if (!Resource) continue;
			if (Resource->Proxy) (void)GetRenderProxy(static_cast<ERHIFeatureLevel>(Index));
			if (Resource->bRequested && !IsDynamicInstance())
				Private::GetMaterialPreparationQueue().Add(FWeakObjectPtr(const_cast<DMaterialInterface*>(this)));
		}
	}

	auto DMaterialInterface::ReleaseFeatureLevelResources() -> void
	{
		CancelFeatureLevelCompilation();
		for (auto& Resource : FeatureLevelResources)
		{
			if (Resource) ReleaseMaterialRenderProxy_GameThread(std::move(Resource->Proxy));
			Resource.reset();
		}
	}

	auto DMaterialInterface::PumpFeatureLevelResources(bool Synchronous) -> bool
	{
		bool Pending = false;
		for (size_t Index = 0; Index < FeatureLevelResources.size(); ++Index)
		{
			const auto Resource = FeatureLevelResources[Index];
			if (!Resource || !Resource->bRequested || IsDynamicInstance()) continue;
			if (!GetAssetRuntimeConfiguration().RequiresCookedPayload()
				&& CompilationState.DefaultResource->Status.HasUnsubmittedEdits()
				&& !Resource->bPending) continue;
			CacheResourceShaders(Resource, static_cast<ERHIFeatureLevel>(Index), Synchronous
				? EMaterialShaderPrecompileMode::Synchronous : EMaterialShaderPrecompileMode::Background);
			Pending |= Resource->bPending || Resource->CookedPreparation;
		}
		return Pending;
	}

	auto DMaterialInterface::CancelFeatureLevelCompilation() -> void
	{
		for (const auto& Resource : FeatureLevelResources)
		{
			if (!Resource) continue;
			const bool Pending = Resource->bPending || Resource->CookedPreparation;
			FShaderCompilingManager::Get().CancelCompilation(Resource);
			Resource->CookedPreparation.reset();
			if (Pending) Resource->Status.State = EMaterialCompileState::Canceled;
			Resource->bRequested = false;
		}
	}

	auto DMaterialInterface::HasFeatureLevelRenderReferences() const -> bool
	{
		return std::ranges::any_of(FeatureLevelResources, [](const auto& Resource) {
			return Resource && Resource->Proxy && Resource->Proxy->GetRefCount() > 1;
		});
	}
}
