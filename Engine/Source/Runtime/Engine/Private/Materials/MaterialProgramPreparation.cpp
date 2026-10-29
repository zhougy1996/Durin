#include "Materials/MaterialInterface.h"
#include "Materials/MaterialCookedProgram.h"
#include "MaterialCompileRetryQueue.h"
#include "Asset/Asset.h"
#include "DObject/WeakObjectPtr.h"
#include "DynamicRHI.h"
#include "RHIGlobals.h"
#include "Threading/Task.h"
#include "Threading/RunnableThread.h"
#include "Shader/ShaderCompilingManager.h"

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
			auto& State = *(CompilationOwner.CookedPreparation = std::make_shared<FMaterialProgramPreparationState>());
			State.BulkRevision = CookedProgramData.GetContentRevision();
			State.CookRoot = GetAssetRuntimeConfiguration().GetCookRoot();
			State.CookTarget = GetAssetRuntimeConfiguration().GetCookTargetPlatform();
			State.Quality = GetMaterialQualityLevel();
			State.FeatureLevel = PreparationFeatureLevel();
			State.Backend = PreparationBackend();
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
		// Admission retires the owner's request, so retain it through publication.
		const auto State = CompilationOwner.CookedPreparation;
		const auto Finish = [&](EMaterialCompileState Status, std::string Diagnostic) {
			CompilationOwner.MaterialCompileStatus.State = Status;
			MaterialCookDiagnostic = std::move(Diagnostic);
			CompilationOwner.CookedPreparation.reset();
			return false;
		};
		const auto Metadata = CookedProgramData.GetMetadata();
		if (!GetAssetRuntimeConfiguration().RequiresCookedPayload()
			|| GetAssetRuntimeConfiguration().GetCookRoot() != State->CookRoot
			|| GetAssetRuntimeConfiguration().GetCookTargetPlatform() != State->CookTarget
			|| (Metadata.Range.Resource && Metadata.Range.Resource->IsRetired())
			|| CookedProgramData.GetContentRevision() != State->BulkRevision
			|| State->Quality != GetMaterialQualityLevel()
			|| State->FeatureLevel != PreparationFeatureLevel() || State->Backend != PreparationBackend())
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
				auto Read = CookedProgramData.AcquireRead();
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
				if (Count >= 16) return true;
				Admission->Active = true;
				FBulkData Snapshot = CookedProgramData;
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
			if (!State->Request->IsReady() && !bSynchronous && IsTaskSchedulerRunning()) return true;
			// Join the existing read/decode even in synchronous mode; never submit it twice.
			const auto Read = State->Request->Wait();
			if (!Read) return Finish(EMaterialCompileState::Failed, FormatPackageResourceReadError(Read));
		}
		if (!State->Result->Program || !State->Result->Diagnostic.empty())
		{
			return Finish(EMaterialCompileState::Failed, State->Result->Diagnostic);
		}
		const auto Admitted = AdmitCookedProgram(State->Result->Properties, State->Result->Program);
		if (!Admitted) return Finish(EMaterialCompileState::Failed, Admitted.error());
		MaterialCookDiagnostic.clear();
		return false;
	}
}
