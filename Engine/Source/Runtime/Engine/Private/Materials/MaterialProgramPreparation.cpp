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
#include "Materials/MaterialCompileTypes.h"
#include "Materials/MaterialShaderMap.h"
#include "RenderingThread.h"

namespace Durin
{
	struct FMaterialProgramDecodeResult
	{
		std::atomic_bool Canceled = false;
		FMaterialStaticProperties Properties;
		std::shared_ptr<const FMaterialProgram> Program;
		std::string Diagnostic;
	};
	// Only request identity and detached work live here. The material owns compile
	// status, diagnostics and the accepted render program.
	struct FMaterialProgramPreparationState
	{
		FMaterialCompilationConfiguration Configuration;
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
		std::shared_ptr<const FMaterialProgram> Program;
	};
	static auto CreateProgramPreparation(const FMaterialCompilationConfiguration& Configuration)
		-> std::shared_ptr<FMaterialProgramPreparationState>
	{
		auto State = std::make_shared<FMaterialProgramPreparationState>();
		State->Configuration = Configuration;
		return State;
	}
	static auto PollProgramPreparation(const DMaterialInterface& Material,
		const std::shared_ptr<FMaterialProgramPreparationState>& State, bool bSynchronous)
		-> std::expected<std::optional<FPreparedMaterialProgram>, std::string>
	{
		const auto Finish = [](std::string Diagnostic)
			-> std::expected<std::optional<FPreparedMaterialProgram>, std::string> {
			return std::unexpected(std::move(Diagnostic));
		};
		const auto Metadata = Material.GetCookedProgramData().GetMetadata();
		if (Metadata.Range.Resource && Metadata.Range.Resource->IsRetired())
			return Finish("Cooked program resource was retired before publication.");
		if (!State->Request)
		{
			if (Metadata.LogicalSize == 0 || Metadata.LogicalSize > MaterialCookedProgramMaxPayloadBytes)
				return Finish("Cooked program preparation requires bounded cooked data.");
			const auto Decode = [Result = State->Result, Quality = State->Configuration.Quality,
				Level = State->Configuration.FeatureLevel, Backend = State->Configuration.Backend](FByteView Bytes) {
				const auto Decoded = DecodeMaterialCookedProgramFamily(Bytes,
					Backend == ERHIBackend::Metal ? ECookTargetPlatform::MacOS : ECookTargetPlatform::Win64,
					ECookTargetProfile::Game, Quality, Level, {}, Result->Properties, Result->Program);
				if (!Decoded) Result->Diagnostic = Decoded.error();
			};
			if (bSynchronous)
			{
				FBulkData Snapshot = Material.GetCookedProgramData();
				auto Read = Snapshot.AcquireRead();
				if (!Read) return Finish(Read.error().Message);
				Decode(Read->GetBytes());
			}
			else
			{
				if (!IsTaskSchedulerRunning())
					return Finish("Cooked program task scheduler stopped before admission.");

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
			if (!Read) return Finish(FormatPackageResourceReadError(Read));
		}
		if (!State->Result->Program || !State->Result->Diagnostic.empty())
		{
			return Finish(State->Result->Diagnostic);
		}
		return FPreparedMaterialProgram{State->Result->Properties, State->Result->Program};
	}
	auto DMaterialInterface::GetCompilationResource(ERHIFeatureLevel Level, EMaterialQualityLevel Quality, bool Create) const
		-> std::shared_ptr<FMaterialResource>
	{
		const FMaterialCompilationConfiguration Key{Level, Quality, PreparationBackend()};
		for (const auto& Resource : CompilationState.Resources)
			if (Resource->Configuration == Key) return Resource;
		if (!Create) return {};
		auto Resource = std::make_shared<FMaterialResource>(Key);
		CompilationState.Resources.push_back(Resource);
		return Resource;
	}

	auto DMaterialInterface::GetRenderingCompilationResource() const -> std::shared_ptr<FMaterialResource>
	{ return GetCompilationResource(PreparationFeatureLevel(), GetMaterialQualityLevel(), true); }

	auto DMaterialInterface::CaptureCompilationInputStamp() const -> FMaterialCompilationInputStamp
	{
		return {CompilationState.AuthoredRevision, GetShaderReloadGeneration(), CookedProgramData.GetContentRevision(),
			GetAssetRuntimeConfiguration().RequiresCookedPayload(), GetAssetRuntimeConfiguration().GetCookRoot(),
			GetAssetRuntimeConfiguration().GetCookTargetPlatform()};
	}

	auto DMaterialInterface::GetResourceCompileStatus(const std::shared_ptr<FMaterialResource>& Resource) const -> FMaterialCompileStatus
	{
		FMaterialCompileStatus Status;
		Status.bHasUnsubmittedEdits = CompilationState.SubmittedRevision != CompilationState.AuthoredRevision;
		if (!Resource) return Status;
		Status.State = Resource->Compilation.expired() ? Resource->Outcome : EMaterialCompileState::Running;
		Status.bCurrent = Resource->CompiledProgram
			&& Resource->CompiledProgram->GetShaderMap().ShaderMapId.IsValid()
			&& Resource->CompiledRevision == CompilationState.AuthoredRevision
			&& Resource->CompiledDependencyRevision == GetShaderReloadGeneration();
		return Status;
	}

	auto DMaterialInterface::GetMaterialContentRevision() const -> uint64
	{
		CheckGameThread();
		if (IsDynamicInstance()) return IsValid(GetParent()) ? GetParent()->GetMaterialContentRevision() : 1;
		return CompilationState.AuthoredRevision;
	}

	auto DMaterialInterface::GetMaterialCompileDebugInfo() const -> FMaterialCompileDebugInfo
	{
		if (IsDynamicInstance()) return IsValid(GetParent()) ? GetParent()->GetMaterialCompileDebugInfo() : FMaterialCompileDebugInfo{};
		const auto& Configuration = CompilationState.SelectedResource->Configuration;
		return GetMaterialCompileDebugInfo(Configuration.FeatureLevel, Configuration.Quality);
	}

	auto DMaterialInterface::GetMaterialCompileDebugInfo(ERHIFeatureLevel Level, EMaterialQualityLevel Quality) const -> FMaterialCompileDebugInfo
	{
		CheckGameThread();
		if (IsDynamicInstance()) return IsValid(GetParent()) ? GetParent()->GetMaterialCompileDebugInfo(Level, Quality) : FMaterialCompileDebugInfo{};
		FMaterialCompileDebugInfo Info;
		Info.AuthoredRevision = CompilationState.AuthoredRevision;
		Info.SubmittedRevision = CompilationState.SubmittedRevision;
		if (const auto Resource = GetCompilationResource(Level, Quality)) {
			Info.CompiledRevision = Resource->CompiledRevision;
			Info.CompiledDependencyRevision = Resource->CompiledDependencyRevision;
			Info.RequestId = Resource->RequestId;
			Info.CacheInfo = Resource->CacheInfo;
			if (const auto Program = Resource->CompiledProgram) {
				Info.CompiledIdentity = Program->GetShaderMap().ShaderMapId;
				Info.Target = Program->GetShaderMap().Target;
			}
			if (const auto Request = Resource->Compilation.lock(); Request && Request->CompilerInput)
				Info.Target = Request->CompilerInput->Environment.Target;
		}
		return Info;
	}

	auto DMaterialInterface::CacheResourceShadersForRendering(EMaterialShaderPrecompileMode Mode) -> void
	{
		CheckGameThread();
		if (Mode == EMaterialShaderPrecompileMode::None || IsPendingKill()) return;
		if (IsDynamicInstance()) { if (IsValid(GetParent())) GetParent()->CacheResourceShadersForRendering(Mode); return; }
		const auto& Key = CompilationState.SelectedResource->Configuration;
		CompilationState.SelectedResource = GetCompilationResource(Key.FeatureLevel, Key.Quality, true);
		CacheResourceShaders(CompilationState.SelectedResource, Mode);
	}

	auto DMaterialInterface::CacheResourceShadersForRendering(ERHIFeatureLevel Level, EMaterialShaderPrecompileMode Mode) -> void
	{ CacheResourceShadersForRendering(Level, GetMaterialQualityLevel(), Mode); }

	auto DMaterialInterface::CacheResourceShadersForRendering(ERHIFeatureLevel Level, EMaterialQualityLevel Quality, EMaterialShaderPrecompileMode Mode) -> void
	{
		CheckGameThread();
		if (Mode == EMaterialShaderPrecompileMode::None || IsPendingKill()
			|| !IsSupportedMaterialFeatureLevel(Level) || !IsSupportedMaterialQualityLevel(Quality)) return;
		if (IsDynamicInstance()) { if (IsValid(GetParent())) GetParent()->CacheResourceShadersForRendering(Level, Quality, Mode); return; }
		CacheResourceShaders(GetCompilationResource(Level, Quality, true), Mode);
	}

	auto DMaterialInterface::CacheResourceShaders(const std::shared_ptr<FMaterialResource>& Resource,
		EMaterialShaderPrecompileMode Mode) -> void
	{
		const bool Synchronous = Mode == EMaterialShaderPrecompileMode::Synchronous;
		Resource->bRequested = true;
		if (Resource->Configuration.FeatureLevel > PreparationFeatureLevel())
		{
			Private::FMaterialCompilationLifecycle::ResetRequest(*this, Resource);
			Resource->Outcome = EMaterialCompileState::Failed;
			Resource->CookDiagnostic = "Material feature level exceeds RHI capability.";
			Resource->bRequested = false;
			Resource->ClearAcceptedProgram();
			MarkRenderDataDirty(EMaterialRenderDirtyFlags::ShaderMap | EMaterialRenderDirtyFlags::PipelineState);
			return;
		}
		for (;;)
		{
			const auto Stamp = CaptureCompilationInputStamp();
			const bool SameInput = Resource->RequestedInput == Stamp;
			if (!SameInput)
			{
				FShaderCompilingManager::Get().CancelCompilation(Resource);
				Resource->CookedPreparation.reset();
				Resource->Outcome = EMaterialCompileState::NeverRequested;
			}
			if (SameInput && !Resource->Compilation.expired())
			{
				if (!Synchronous) return;
				FShaderCompilingManager::Get().FinishResourceCompilation(Resource);
			}
			else if (SameInput && Resource->Outcome == EMaterialCompileState::Ready)
			{ Resource->bRequested = false; return; }
			else
			{
				if (Stamp.bCooked)
				{
					Private::FMaterialCompilationLifecycle::ResetRequest(*this, Resource);
					Resource->Material = FWeakObjectPtr(this);
					auto Request = std::make_shared<FMaterialCompileRequest>(FMaterialCompileRequest{.Resource = Resource});
					Resource->Compilation = Request;
					FShaderCompilingManager::Get().RegisterMaterialRequest(Request);
					Resource->CookedPreparation = CreateProgramPreparation(Resource->Configuration);
					PollResourcePreparation(Resource, Synchronous);
				}
				else
				{
					FMaterialCompilerEnvironment Environment;
					const auto Built = BuildDefaultMaterialCompilerEnvironment(Environment);
					if (!Built)
					{
						Private::FMaterialCompilationLifecycle::ResetRequest(*this, Resource);
						Resource->Outcome = EMaterialCompileState::Failed;
						Resource->Diagnostics = {{.Message = Built.error()}};
						Resource->ClearAcceptedProgram();
						Resource->bRequested = false;
						MarkRenderDataDirty(EMaterialRenderDirtyFlags::ShaderMap | EMaterialRenderDirtyFlags::PipelineState);
						return;
					}
					RequestProgramCompile(GetStaticProperties(), false, nullptr, std::move(Environment), Resource);
				}
				if (!Resource->Compilation.expired())
				{
					Private::GetMaterialPreparationQueue().Add(FWeakObjectPtr(this));
					if (Synchronous) FShaderCompilingManager::Get().FinishResourceCompilation(Resource);
				}
			}
			if (!Synchronous || Stamp == CaptureCompilationInputStamp()) return;
		}
	}

	auto DMaterialInterface::PollResourcePreparation(const std::shared_ptr<FMaterialResource>& Resource, bool Synchronous) -> void
	{
		if (!Resource->CookedPreparation) return;
		const auto Prepared = PollProgramPreparation(*this, Resource->CookedPreparation, Synchronous);
		if (Prepared && !*Prepared) return;
		FMaterialCompileResult Result{.Request = Resource->Compilation.lock()};
		if (!Prepared)
		{
			Resource->CookDiagnostic = Prepared.error();
			Result.Diagnostics = {{.Message = Prepared.error()}};
		}
		else
		{
			Result.StaticProperties = (*Prepared)->Properties;
			Result.CompiledProgram = (*Prepared)->Program;
			Result.Outcome = EMaterialProgramStatus::Ok;
		}
		Resource->CookedPreparation.reset();
		FShaderCompilingManager::Get().QueueMaterialResult(std::move(Result));
	}

	auto DMaterialInterface::GetAcceptedCompiledProgram(ERHIFeatureLevel Level, EMaterialQualityLevel Quality) const -> std::shared_ptr<const FMaterialProgram>
	{
		if (!IsSupportedMaterialFeatureLevel(Level) || !IsSupportedMaterialQualityLevel(Quality)) return {};
		if (IsDynamicInstance()) return IsValid(GetParent()) ? GetParent()->GetAcceptedCompiledProgram(Level, Quality) : nullptr;
		const auto Resource = GetCompilationResource(Level, Quality);
		return Resource ? Resource->CompiledProgram : nullptr;
	}

	auto DMaterialInterface::GetMaterialResource(ERHIFeatureLevel Level, EMaterialQualityLevel Quality) const -> const FMaterialResource*
	{
		if (!IsSupportedMaterialFeatureLevel(Level) || !IsSupportedMaterialQualityLevel(Quality)) return nullptr;
		if (IsDynamicInstance()) return IsValid(GetParent()) ? GetParent()->GetMaterialResource(Level, Quality) : nullptr;
		return GetCompilationResource(Level, Quality).get();
	}
	auto DMaterialInterface::GetMaterialResourceProperties(ERHIFeatureLevel Level, EMaterialQualityLevel Quality) const -> FMaterialStaticProperties
	{
		const auto* Resource = GetMaterialResource(Level, Quality);
		return Resource ? ResolveResourceProperties(*Resource) : FMaterialStaticProperties{};
	}
	auto DMaterialInterface::GetMaterialRenderParameters(ERHIFeatureLevel Level, EMaterialQualityLevel Quality) const -> FMaterialInstanceParameterSet
	{
		const auto* Resource = GetMaterialResource(Level, Quality);
		return Resource ? BuildMaterialRenderParameters(*Resource) : FMaterialInstanceParameterSet{};
	}

	auto DMaterialInterface::GetMaterialCompileStatus(ERHIFeatureLevel Level, EMaterialQualityLevel Quality) const -> FMaterialCompileStatus
	{
		if (!IsSupportedMaterialFeatureLevel(Level) || !IsSupportedMaterialQualityLevel(Quality)) return {.State = EMaterialCompileState::Failed};
		if (IsDynamicInstance()) return IsValid(GetParent()) ? GetParent()->GetMaterialCompileStatus(Level, Quality) : FMaterialCompileStatus{};
		auto Status = GetResourceCompileStatus(GetCompilationResource(Level, Quality));
		return Status;
	}

	auto DMaterialInterface::GetMaterialCookDiagnostic(ERHIFeatureLevel Level, EMaterialQualityLevel Quality) const -> std::string
	{
		if (!IsSupportedMaterialFeatureLevel(Level) || !IsSupportedMaterialQualityLevel(Quality)) return "Unsupported material configuration.";
		if (IsDynamicInstance()) return IsValid(GetParent()) ? GetParent()->GetMaterialCookDiagnostic(Level, Quality) : "Missing material parent.";
		const auto Resource = GetCompilationResource(Level, Quality);
		if (!Resource) return {};
		if (!Resource->CookDiagnostic.empty()) return Resource->CookDiagnostic;
		return Resource->Diagnostics.empty() ? std::string{} : FormatMaterialProgramDiagnostics(Resource->Diagnostics, "Material compilation failed.");
	}

	auto DMaterialInterface::GetRenderProxy(ERHIFeatureLevel Level, EMaterialQualityLevel Quality) const -> FMaterialRenderProxyRef
	{
		CheckGameThread();
		if (IsTemplateObject() || !bAcceptingMaterialProxyPublications
			|| !IsSupportedMaterialFeatureLevel(Level) || !IsSupportedMaterialQualityLevel(Quality)) return {};
		const auto Resource = GetCompilationResource(Level, Quality, true);
		if (!Resource->Proxy) { Resource->Proxy = MakeRefCount<FMaterialRenderProxy>(); Resource->bProxyInitialized = false; }
		if (!Resource->bProxyInitialized && GetRenderCommandAdmissionState() == ERenderCommandAdmissionState::Running)
		{
			SubmitMaterialProxyUpdates(*Resource->Proxy, GetMaterialResource(Level, Quality), EMaterialRenderDirtyFlags::AllRenderState);
			Resource->bProxyInitialized = true;
		}
		if (GetRenderCommandAdmissionState() == ERenderCommandAdmissionState::Running)
			Resource->Proxy->SetCompiling_GameThread(GetMaterialCompileStatus(Level, Quality).State == EMaterialCompileState::Running);
		return Resource->Proxy;
	}

	auto DMaterialInterface::PublishCompilationResources(EMaterialRenderDirtyFlags Flags, std::span<const FGuid> ChangedParameters) const -> void
	{
		if (!bAcceptingMaterialProxyPublications) return;
		for (const auto& Resource : CompilationState.Resources)
		{
			// Defaults belong to the immutable program, never to the resource or proxy.
			if (!IsDynamicInstance() && Resource->CompiledProgram)
			{
				if (EnumHasAnyFlags(Flags, EMaterialRenderDirtyFlags::ShaderMap
					| EMaterialRenderDirtyFlags::ParentChain | EMaterialRenderDirtyFlags::DynamicParameters))
					Resource->CompiledProgram = BindMaterialDefaultParameters(Resource->CompiledProgram);
				if (EnumHasAnyFlags(Flags, EMaterialRenderDirtyFlags::ShaderMap
					| EMaterialRenderDirtyFlags::ParentChain | EMaterialRenderDirtyFlags::PipelineState))
					Resource->AcceptedProperties = ResolveResourceProperties(*Resource);
			}
			if (Resource->Proxy && Resource->Configuration.Backend == PreparationBackend()
				&& GetRenderCommandAdmissionState() == ERenderCommandAdmissionState::Running)
			{
				SubmitMaterialProxyUpdates(*Resource->Proxy, GetMaterialResource(Resource->Configuration.FeatureLevel, Resource->Configuration.Quality),
					Resource->bProxyInitialized ? Flags : EMaterialRenderDirtyFlags::AllRenderState, ChangedParameters);
				Resource->bProxyInitialized = true;
			}
			if (!IsDynamicInstance() && (Resource->bRequested || !Resource->Compilation.expired()
				|| (Resource->Proxy && Resource->Proxy->GetRefCount() > 1)))
				Private::GetMaterialPreparationQueue().Add(FWeakObjectPtr(const_cast<DMaterialInterface*>(this)));
		}
	}

	auto DMaterialInterface::ReleaseCompilationResources() -> void
	{
		CancelResourceCompilation();
		for (const auto& Resource : CompilationState.Resources)
			ReleaseMaterialRenderProxy_GameThread(std::move(Resource->Proxy));
	}

	auto DMaterialInterface::PumpResourcePreparations(const FAssetCompileProcessParams& Params) -> bool
	{
		bool Pending = false;
		// Capture resources: starting a request may select or allocate another configuration.
		const auto Resources = CompilationState.Resources;
		for (const auto& Resource : Resources)
		{
			if (Params.Deadline && std::chrono::steady_clock::now() >= *Params.Deadline) return true;
			if (IsDynamicInstance()) continue;
			const bool Demanded = Resource->bRequested || (Resource->Proxy && Resource->Proxy->GetRefCount() > 1);
			if (Demanded && Resource->RequestedInput != CaptureCompilationInputStamp()
				&& CompilationState.SubmittedRevision == CompilationState.AuthoredRevision)
				CacheResourceShaders(Resource, EMaterialShaderPrecompileMode::Background);
			PollResourcePreparation(Resource, false);
			Pending |= !Resource->Compilation.expired();
		}
		return Pending;
	}

	auto DMaterialInterface::CancelResourceCompilation() -> void
	{
		for (const auto& Resource : CompilationState.Resources)
		{
			if (!Resource->Compilation.expired()) Resource->Outcome = EMaterialCompileState::Canceled;
			FShaderCompilingManager::Get().CancelCompilation(Resource);
			Resource->CookedPreparation.reset();
			Resource->bRequested = false;
		}
	}

	auto DMaterialInterface::HasResourceRenderReferences() const -> bool
	{
		return std::ranges::any_of(CompilationState.Resources, [](const auto& Resource) {
			return Resource->Proxy && Resource->Proxy->GetRefCount() > 1;
		});
	}
}
