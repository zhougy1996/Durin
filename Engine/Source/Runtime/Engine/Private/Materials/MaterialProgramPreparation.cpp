#include "Materials/MaterialInterface.h"
#include "Materials/MaterialCookedProgram.h"
#include "MaterialCompileRetryQueue.h"
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
	static std::atomic_uint32_t ActiveProgramPreparations = 0;
	// Admission belongs to the running continuation, never to a retained owner.
	struct FMaterialProgramAdmission
	{
		std::atomic_bool Active = false;
		auto Release() -> void { if (Active.exchange(false)) --ActiveProgramPreparations; }
		~FMaterialProgramAdmission() { Release(); }
	};
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
				auto Admission = std::make_shared<FMaterialProgramAdmission>();
				auto Count = ActiveProgramPreparations.load();
				while (Count < 16 && !ActiveProgramPreparations.compare_exchange_weak(Count, Count + 1)) {}
				if (Count >= 16) return std::optional<FPreparedMaterialProgram>{};
				Admission->Active = true;
				FBulkData Snapshot = Material.GetCookedProgramData();
				State->Request = FPackageResourceRequest::Transform(Snapshot.ReloadAsync(),
					[Admission = std::move(Admission), Result = State->Result, Decode](FPackageResourceReadResult Read) {
						struct FReleaseAdmission
						{
							FMaterialProgramAdmission& Admission;
							~FReleaseAdmission() { Admission.Release(); }
						} Release{*Admission};
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
	auto DMaterialInterface::CacheResourceShadersForRendering(
		EMaterialShaderPrecompileMode PrecompileMode) -> void
	{
		CheckGameThread();
		if (PrecompileMode == EMaterialShaderPrecompileMode::None || IsPendingKill()) return;
		if (IsDynamicInstance())
		{
			if (auto* Parent = GetParent()) Parent->CacheResourceShadersForRendering(PrecompileMode);
			return;
		}
		if (IsComplete()) return;
		const bool bSynchronous = PrecompileMode == EMaterialShaderPrecompileMode::Synchronous;
		if (!GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (!IsCompiling()) RequestMaterialRecompile(*this);
			if (bSynchronous)
			{
				const std::array<DObject*, 1> Owners{this};
				FShaderCompilingManager::Get().FinishCompilationForObjects(Owners);
			}
			return;
		}
		if (!CompilationOwner.CookedPreparation)
		{
			auto& Status = CompilationOwner.MaterialCompileStatus.State;
			MaterialCookDiagnostic.clear();
			if (!bSynchronous && !IsTaskSchedulerRunning())
			{
				Status = EMaterialCompileState::Failed;
				MaterialCookDiagnostic = "Background cooked program preparation requires a running task scheduler.";
				return;
			}
			CompilationOwner.CookedPreparation = CreateProgramPreparation(*this, PreparationFeatureLevel());
			Status = EMaterialCompileState::Running;
		}
		if (bSynchronous)
		{
			PollCookedProgramPreparation(true);
			Private::GetMaterialCompileRetryQueue().Remove(FWeakObjectPtr(this));
		}
		else Private::GetMaterialCompileRetryQueue().Add(FWeakObjectPtr(this));
	}
	auto DMaterialInterface::PollCookedProgramPreparation(bool bSynchronous) -> bool
	{
		CheckGameThread();
		if (!CompilationOwner.CookedPreparation) return false;
		const auto State = CompilationOwner.CookedPreparation;
		if (State->FeatureLevel != PreparationFeatureLevel())
		{
			CompilationOwner.MaterialCompileStatus.State = EMaterialCompileState::Canceled;
			MaterialCookDiagnostic = "Cooked program feature level changed before publication.";
			CompilationOwner.CookedPreparation.reset();
			return false;
		}
		const auto Result = PollProgramPreparation(*this, State, bSynchronous);
		if (!Result)
		{
			CompilationOwner.MaterialCompileStatus.State = Result.error().State;
			MaterialCookDiagnostic = Result.error().Diagnostic;
			CompilationOwner.CookedPreparation.reset();
			return false;
		}
		if (!*Result) return true;
		const auto Admitted = AdmitCookedProgram((*Result)->Properties, (*Result)->Program);
		if (!Admitted)
		{
			CompilationOwner.MaterialCompileStatus.State = EMaterialCompileState::Failed;
			MaterialCookDiagnostic = Admitted.error();
			CompilationOwner.CookedPreparation.reset();
		}
		else MaterialCookDiagnostic.clear();
		return false;
	}
	struct FMaterialFeatureLevelMailbox
	{
		std::mutex Mutex;
		std::optional<MaterialBuildPrivate::FMaterialBuildResult> Result;
	};
	// One resource per supported tier, using the existing shared compiler and cooked reader.
	// Completion mailboxes never retain a material or publish from a worker.
	struct FMaterialFeatureLevelResource
	{
		FMaterialResource Resource;
		FMaterialCompileStatus Status;
		std::string Diagnostic;
		uint64 AuthoredRevision = 0, ParentRevision = 0, DependencyRevision = 0, ShaderRevision = 0, BulkRevision = 0;
		ERHIBackend Backend = PreparationBackend();
		bool bCooked = false;
		std::filesystem::path CookRoot;
		ECookTargetPlatform CookTarget = ECookTargetPlatform::Win64;
		EMaterialQualityLevel Quality = EMaterialQualityLevel::High;
		std::vector<FMaterialFunctionOwnerStamp> Functions;
		FMaterialStaticProperties Properties;
		std::shared_ptr<FMaterialProgramPreparationState> Cooked;
		std::shared_ptr<FMaterialFeatureLevelMailbox> Mailbox;
		MaterialBuildPrivate::FMaterialProgramCompileHandle Request;
		FCancellationToken Cancellation;
		FMaterialRenderProxyRef Proxy;
		bool bRequested = false;
		~FMaterialFeatureLevelResource() { Cancellation.Cancel(); }
	};

	auto DMaterialInterface::GetAcceptedCompiledProgram(ERHIFeatureLevel Level) const
		-> std::shared_ptr<const FMaterialCompilerResult>
	{
		if (!IsSupportedMaterialFeatureLevel(Level)) return {};
		if (IsDynamicInstance()) return IsValid(GetParent()) ? GetParent()->GetAcceptedCompiledProgram(Level) : nullptr;
		const auto Program = GetAcceptedCompiledProgram();
		if (Program && Program->FeatureLevel == Level) return Program;
		if (!Program && Level == PreparationFeatureLevel()) return {};
		const auto& State = FeatureLevelResources[static_cast<size_t>(Level)];
		return State ? State->Resource.CompiledProgram : nullptr;
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
		const auto Program = GetAcceptedCompiledProgram();
		if (Program && Program->FeatureLevel == Level) return BuildMaterialResource();
		if (!Program && Level == PreparationFeatureLevel()) return {.bIsCompiling = IsCompiling()};
		const auto& State = FeatureLevelResources[static_cast<size_t>(Level)];
		if (!State) return {};
		auto Resource = State->Resource;
		const auto& Authored = GetStaticProperties();
		if (Resource.StaticProperties && CanonicalizeMaterialShaderProperties(*Resource.StaticProperties) == CanonicalizeMaterialShaderProperties(Authored))
		{
			Resource.StaticProperties->bTwoSided = Authored.bTwoSided;
			Resource.StaticProperties->DepthWritePolicy = Authored.DepthWritePolicy;
		}
		Resource.bIsCompiling = State->Status.State == EMaterialCompileState::Running || State->Status.State == EMaterialCompileState::Deferred;
		return BuildMaterialResource(std::move(Resource));
	}

	auto DMaterialInterface::GetMaterialCompileStatus(ERHIFeatureLevel Level) const -> FMaterialCompileStatus
	{
		if (!IsSupportedMaterialFeatureLevel(Level)) return {.State = EMaterialCompileState::Rejected};
		if (IsDynamicInstance()) return IsValid(GetParent()) ? GetParent()->GetMaterialCompileStatus(Level) : FMaterialCompileStatus{};
		const auto Program = GetAcceptedCompiledProgram();
		if ((Program && Program->FeatureLevel == Level) || (!Program && Level == PreparationFeatureLevel())) return GetMaterialCompileStatus();
		const auto& State = FeatureLevelResources[static_cast<size_t>(Level)];
		return State ? State->Status : FMaterialCompileStatus{};
	}

	auto DMaterialInterface::GetMaterialCookDiagnostic(ERHIFeatureLevel Level) const -> std::string
	{
		if (!IsSupportedMaterialFeatureLevel(Level)) return "Unsupported material feature level.";
		if (IsDynamicInstance()) return IsValid(GetParent()) ? GetParent()->GetMaterialCookDiagnostic(Level) : "Missing material parent.";
		const auto Program = GetAcceptedCompiledProgram();
		if ((Program && Program->FeatureLevel == Level) || (!Program && Level == PreparationFeatureLevel())) return GetMaterialCookDiagnostic();
		const auto& State = FeatureLevelResources[static_cast<size_t>(Level)];
		return State ? State->Diagnostic : GetMaterialCookDiagnostic();
	}

	auto DMaterialInterface::GetRenderProxy(ERHIFeatureLevel Level) const -> FMaterialRenderProxyRef
	{
		CheckGameThread();
		if (IsTemplateObject() || !bAcceptingMaterialProxyPublications || !IsSupportedMaterialFeatureLevel(Level)) return {};
		auto& State = FeatureLevelResources[static_cast<size_t>(Level)];
		if (!State) State = std::make_shared<FMaterialFeatureLevelResource>();
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
			const auto& State = FeatureLevelResources[Index];
			if (!State) continue;
			const auto Level = static_cast<ERHIFeatureLevel>(Index);
			if (State->Proxy) (void)GetRenderProxy(Level);
			if (State->bRequested && !IsDynamicInstance())
				Private::GetMaterialCompileRetryQueue().Add(FWeakObjectPtr(const_cast<DMaterialInterface*>(this)));
		}
	}

	auto DMaterialInterface::ReleaseFeatureLevelResources() -> void
	{
		for (auto& State : FeatureLevelResources)
		{
			if (State) ReleaseMaterialRenderProxy_GameThread(std::move(State->Proxy));
			State.reset();
		}
	}

	auto DMaterialInterface::CacheResourceShadersForRendering(ERHIFeatureLevel Level,
		EMaterialShaderPrecompileMode Mode) -> void
	{
		CheckGameThread();
		if (Mode == EMaterialShaderPrecompileMode::None || IsPendingKill() || !IsSupportedMaterialFeatureLevel(Level)) return;
		if (IsDynamicInstance()) { if (IsValid(GetParent())) GetParent()->CacheResourceShadersForRendering(Level, Mode); return; }
		auto& Slot = FeatureLevelResources[static_cast<size_t>(Level)];
		if (!Slot) Slot = std::make_shared<FMaterialFeatureLevelResource>();
		if (Slot->Status.State == EMaterialCompileState::Canceled)
		{
			auto Replacement = std::make_shared<FMaterialFeatureLevelResource>();
			Replacement->Resource = std::move(Slot->Resource);
			Replacement->Proxy = std::move(Slot->Proxy);
			Slot = std::move(Replacement);
		}
		Slot->bRequested = true;
		const bool bSynchronous = Mode == EMaterialShaderPrecompileMode::Synchronous;
		const auto Primary = GetAcceptedCompiledProgram();
		if (Level == PreparationFeatureLevel() && (!Primary || Primary->FeatureLevel == Level)) CacheResourceShadersForRendering(Mode);
		const auto SelectedPrimary = GetAcceptedCompiledProgram();
		if (SelectedPrimary && SelectedPrimary->FeatureLevel == Level)
		{
			Slot->Resource = BuildMaterialResource();
			Slot->Status = GetMaterialCompileStatus();
			Slot->AuthoredRevision = CompilationOwner.MaterialCompileStatus.AuthoredRevision;
			Slot->ParentRevision = CompilationOwner.MaterialCompileStatus.ParentChainRevision;
			Slot->DependencyRevision = CompilationOwner.MaterialCompileStatus.DependencyRevision;
			Slot->ShaderRevision = GetShaderReloadGeneration();
			Slot->BulkRevision = CookedProgramData.GetContentRevision();
			Slot->Quality = GetMaterialQualityLevel();
			Slot->Backend = PreparationBackend();
			Slot->bCooked = GetAssetRuntimeConfiguration().RequiresCookedPayload();
			Slot->CookRoot = GetAssetRuntimeConfiguration().GetCookRoot();
			Slot->CookTarget = GetAssetRuntimeConfiguration().GetCookTargetPlatform();
			return;
		}
		if (Level == PreparationFeatureLevel() && !SelectedPrimary) return;
		const auto& OwnerStatus = CompilationOwner.MaterialCompileStatus;
		const bool bCurrent = Slot->AuthoredRevision == OwnerStatus.AuthoredRevision && Slot->ParentRevision == OwnerStatus.ParentChainRevision
			&& Slot->DependencyRevision == OwnerStatus.DependencyRevision
			&& Slot->ShaderRevision == GetShaderReloadGeneration() && Slot->Quality == GetMaterialQualityLevel()
			&& Slot->BulkRevision == CookedProgramData.GetContentRevision() && AreMaterialFunctionOwnersCurrent(Slot->Functions)
			&& Slot->Backend == PreparationBackend() && Slot->bCooked == GetAssetRuntimeConfiguration().RequiresCookedPayload()
			&& Slot->CookRoot == GetAssetRuntimeConfiguration().GetCookRoot()
			&& Slot->CookTarget == GetAssetRuntimeConfiguration().GetCookTargetPlatform();
		if (!bCurrent)
		{
			auto Replacement = std::make_shared<FMaterialFeatureLevelResource>();
			Replacement->Resource = std::move(Slot->Resource);
			Replacement->Proxy = std::move(Slot->Proxy);
			Replacement->bRequested = true;
			Slot = std::move(Replacement);
			Slot->AuthoredRevision = OwnerStatus.AuthoredRevision;
			Slot->ParentRevision = OwnerStatus.ParentChainRevision;
			Slot->DependencyRevision = OwnerStatus.DependencyRevision;
			Slot->ShaderRevision = GetShaderReloadGeneration();
			Slot->BulkRevision = CookedProgramData.GetContentRevision();
			Slot->Quality = GetMaterialQualityLevel();
			Slot->Backend = PreparationBackend();
			Slot->bCooked = GetAssetRuntimeConfiguration().RequiresCookedPayload();
			Slot->CookRoot = GetAssetRuntimeConfiguration().GetCookRoot();
			Slot->CookTarget = GetAssetRuntimeConfiguration().GetCookTargetPlatform();
			Slot->Status.AuthoredRevision = OwnerStatus.AuthoredRevision;
			Slot->Status.ParentChainRevision = OwnerStatus.ParentChainRevision;
			Slot->Status.DependencyRevision = OwnerStatus.DependencyRevision;
		}
		const auto State = Slot;
		auto Fail = [&](EMaterialCompileState Status, std::string Diagnostic) {
			State->Status.State = Status; State->Diagnostic = std::move(Diagnostic);
			State->Resource = {}; State->Request.reset(); State->Cooked.reset(); State->Mailbox.reset();
			MarkRenderDataDirty(EMaterialRenderDirtyFlags::ShaderMap | EMaterialRenderDirtyFlags::PipelineState);
		};
		if (Level > PreparationFeatureLevel())
		{
			if (State->Status.State != EMaterialCompileState::Rejected || State->Resource.CompiledProgram)
				Fail(EMaterialCompileState::Rejected, "Material feature level exceeds RHI capability.");
			return;
		}
		if (State->Status.State == EMaterialCompileState::Ready || State->Status.State == EMaterialCompileState::Failed
			|| State->Status.State == EMaterialCompileState::Rejected || State->Status.State == EMaterialCompileState::Canceled) return;
		std::shared_ptr<const FMaterialCompilerResult> Program;
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (!State->Cooked) State->Cooked = CreateProgramPreparation(*this, Level);
			State->Status.State = EMaterialCompileState::Running;
			const auto Prepared = PollProgramPreparation(*this, State->Cooked, bSynchronous);
			if (!Prepared) { Fail(Prepared.error().State, Prepared.error().Diagnostic); return; }
			if (*Prepared) { Program = (*Prepared)->Program; State->Properties = (*Prepared)->Properties; State->Cooked.reset(); }
		}
		else
		{
			if (!State->Request)
			{
				if (!bSynchronous && !IsTaskSchedulerRunning()) { State->Status.State = EMaterialCompileState::Deferred; }
				else
				{
					FMaterialCompilerEnvironment Environment;
					const auto Built = BuildDefaultMaterialCompilerEnvironment(Environment);
					if (!Built) { Fail(EMaterialCompileState::Rejected, Built.error()); return; }
					Environment.FeatureLevel = Level;
					auto Snapshot = SnapshotMaterialCompilerInput(*this, std::move(Environment));
					if (!Snapshot) { Fail(EMaterialCompileState::Rejected, Snapshot.Diagnostics.empty() ? "Material capture failed." : Snapshot.Diagnostics.front().Message); return; }
					State->Properties = Snapshot.Snapshot->Input.StaticProperties;
					State->Functions = std::move(Snapshot.Snapshot->FunctionOwners);
					State->Mailbox = std::make_shared<FMaterialFeatureLevelMailbox>();
					const auto Requested = MaterialBuildPrivate::FMaterialCompilation::Compile(
						std::make_shared<const MIR::FCompilerInput>(std::move(Snapshot.Snapshot->Input)), {.Cancellation = State->Cancellation},
						[Weak = std::weak_ptr(State->Mailbox)](MaterialBuildPrivate::FMaterialBuildResult Result) {
							if (const auto Mailbox = Weak.lock()) { std::lock_guard Lock(Mailbox->Mutex); Mailbox->Result = std::move(Result); }
						});
					if (!Requested)
					{
						State->Mailbox.reset();
						if (Requested.error() != MaterialBuildPrivate::EMaterialCompileAdmissionError::Capacity)
						{ Fail(EMaterialCompileState::Rejected, "Material feature-level compile admission failed."); return; }
						State->Status.State = EMaterialCompileState::Deferred;
					}
					else { State->Request = *Requested; State->Status.State = EMaterialCompileState::Running; }
				}
			}
			if (State->Request && bSynchronous) FShaderCompilingManager::Get().FinishCompilation(State->Request);
			if (State->Mailbox)
			{
				std::optional<MaterialBuildPrivate::FMaterialBuildResult> Result;
				{ std::lock_guard Lock(State->Mailbox->Mutex); Result = std::move(State->Mailbox->Result); State->Mailbox->Result.reset(); }
				if (Result)
				{
					if (!*Result) {
						const bool Canceled = State->Cancellation.IsCancellationRequested()
							|| (Result->Program && Result->Program->Status == EMaterialProgramStatus::Canceled);
						Fail(Canceled ? EMaterialCompileState::Canceled : EMaterialCompileState::Failed,
							Result->Program && !Result->Program->Diagnostics.empty() ? Result->Program->Diagnostics.front().Message : "Material feature-level program compilation failed.");
						return;
					}
					Program = Result->Program; State->Request.reset(); State->Mailbox.reset();
				}
			}
		}
		if (!Program) { Private::GetMaterialCompileRetryQueue().Add(FWeakObjectPtr(this)); return; }
		if (Program->FeatureLevel != Level || !ValidateMaterialCompilerResult(*Program))
		{ Fail(EMaterialCompileState::Failed, "Prepared material program has a mismatched feature level or invalid contract."); return; }
		const auto Validated = ValidatePreparedMaterialProgram(State->Properties, std::move(Program));
		if (!Validated) { Fail(EMaterialCompileState::Failed, Validated.error()); return; }
		State->Resource.CompiledProgram = *Validated;
		State->Resource.StaticProperties = State->Properties;
		State->Resource = BuildMaterialResource(State->Resource);
		State->Status.State = EMaterialCompileState::Ready;
		State->Status.CompiledIdentity = State->Status.RequestedIdentity = (*Validated)->GetShaderMap().ShaderMapId;
		State->Status.Target = (*Validated)->GetShaderMap().Target;
		State->Diagnostic.clear();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ShaderMap | EMaterialRenderDirtyFlags::PipelineState);
	}

	auto DMaterialInterface::PumpFeatureLevelResources(bool bSynchronous) -> bool
	{
		bool Pending = false;
		for (size_t Index = 0; Index < FeatureLevelResources.size(); ++Index)
		{
			const auto State = FeatureLevelResources[Index];
			if (!State || !State->bRequested || IsDynamicInstance()) continue;
			CacheResourceShadersForRendering(static_cast<ERHIFeatureLevel>(Index), bSynchronous
				? EMaterialShaderPrecompileMode::Synchronous : EMaterialShaderPrecompileMode::Background);
			const auto Status = FeatureLevelResources[Index]->Status.State;
			Pending |= Status == EMaterialCompileState::Running || Status == EMaterialCompileState::Deferred;
		}
		return Pending;
	}

	auto DMaterialInterface::CancelFeatureLevelCompilation() -> void
	{
		for (const auto& State : FeatureLevelResources)
		{
			if (!State || (!State->Request && !State->Cooked && State->Status.State != EMaterialCompileState::Deferred)) continue;
			State->Cancellation.Cancel();
			State->Request.reset(); State->Cooked.reset(); State->Mailbox.reset();
			State->Status.State = EMaterialCompileState::Canceled;
			State->bRequested = false;
		}
	}

	auto DMaterialInterface::HasFeatureLevelRenderReferences() const -> bool
	{
		return std::ranges::any_of(FeatureLevelResources, [](const auto& State) {
			return State && State->Proxy && State->Proxy->GetRefCount() > 1;
		});
	}

}
