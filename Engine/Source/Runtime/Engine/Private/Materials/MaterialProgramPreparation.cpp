#include "Materials/MaterialProgramPreparation.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialCookedProgram.h"
#include "Asset/Asset.h"
#include "DObject/WeakObjectPtr.h"
#include "DObject/ObjectLifecycle.h"
#include "DynamicRHI.h"
#include "RHIGlobals.h"
#include "Threading/Task.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	static std::atomic_uint32_t ActiveProgramPreparations = 0;
	// Admission belongs to the running continuation, never to a retained observer.
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
	struct FMaterialProgramPreparationState
	{
		FWeakObjectPtr Owner;
		uint64 BulkRevision = 0;
		FBulkDataMetadata Metadata;
		std::filesystem::path CookRoot;
		ECookTargetPlatform CookTarget = ECookTargetPlatform::Win64;
		EMaterialQualityLevel Quality;
		ERHIFeatureLevel FeatureLevel;
		ERHIBackend Backend;
		EMaterialProgramPreparationState Status = EMaterialProgramPreparationState::Pending;
		FPackageResourceRequest Request;
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
	auto FMaterialProgramPreparation::Request(DMaterialInterface& Material) -> FMaterialProgramPreparation
	{
		CheckGameThread();
		FMaterialProgramPreparation Preparation;
		Preparation.State = std::make_shared<FMaterialProgramPreparationState>();
		auto& State = *Preparation.State;
		State.Owner = FWeakObjectPtr(&Material);
		State.BulkRevision = Material.CookedProgramData.GetContentRevision();
		State.Metadata = Material.CookedProgramData.GetMetadata();
		State.CookRoot = GetAssetRuntimeConfiguration().GetCookRoot();
		State.CookTarget = GetAssetRuntimeConfiguration().GetCookTargetPlatform();
		State.Quality = GetMaterialQualityLevel(); State.FeatureLevel = PreparationFeatureLevel(); State.Backend = PreparationBackend();
		if (!GetAssetRuntimeConfiguration().RequiresCookedPayload() || !IsTaskSchedulerRunning()
			|| State.Metadata.LogicalSize == 0 || State.Metadata.LogicalSize > MaterialCookedProgramMaxPayloadBytes)
		{
			State.Status = EMaterialProgramPreparationState::Failed;
			State.Result->Diagnostic = "Cooked program preparation requires bounded cooked data and a task scheduler.";
			return Preparation;
		}
		auto Admission = std::make_shared<FMaterialProgramAdmission>();
		auto Count = ActiveProgramPreparations.load();
		while (Count < 16 && !ActiveProgramPreparations.compare_exchange_weak(Count, Count + 1)) {}
		if (Count >= 16)
		{
			State.Status = EMaterialProgramPreparationState::Failed;
			State.Result->Diagnostic = "Cooked program preparation capacity exceeded.";
			return Preparation;
		}
		Admission->Active = true;
		// Snapshot owns only bytes/range metadata. Workers never dereference Owner.
		FBulkData Snapshot = Material.CookedProgramData;
		State.Request = FPackageResourceRequest::Transform(Snapshot.ReloadAsync(),
			[Admission = std::move(Admission), Result = State.Result, Quality = State.Quality, Level = State.FeatureLevel, Backend = State.Backend](FPackageResourceReadResult Read) {
				struct FReleaseAdmission
				{
					FMaterialProgramAdmission& Admission;
					~FReleaseAdmission() { Admission.Release(); }
				} Release{*Admission};
				if (Result->Canceled) return Read;
				if (!Read) { Result->Diagnostic = FormatPackageResourceReadError(Read); return Read; }
				const auto Decoded = DecodeMaterialCookedProgramFamily(Read->GetBytes(),
					Backend == ERHIBackend::Metal ? ECookTargetPlatform::MacOS : ECookTargetPlatform::Win64,
					ECookTargetProfile::Game, Quality, Level, {}, Result->Properties, Result->Program);
				if (!Decoded) Result->Diagnostic = Decoded.error();
				return Read;
			});
		return Preparation;
	}
	auto FMaterialProgramPreparation::Poll() -> EMaterialProgramPreparationState
	{
		CheckGameThread();
		if (!State) return EMaterialProgramPreparationState::Failed;
		if (State->Status != EMaterialProgramPreparationState::Pending) return State->Status;
		if (State->Result->Canceled) return State->Status = EMaterialProgramPreparationState::Canceled;
		auto* Owner = Cast<DMaterialInterface>(State->Owner.Get());
		const auto Current = Owner ? Owner->CookedProgramData.GetMetadata() : FBulkDataMetadata{};
		const auto& Earlier = State->Metadata;
		if (!IsValid(Owner) || !GetAssetRuntimeConfiguration().RequiresCookedPayload()
			|| GetAssetRuntimeConfiguration().GetCookRoot() != State->CookRoot
			|| GetAssetRuntimeConfiguration().GetCookTargetPlatform() != State->CookTarget
			|| (Current.Range.Resource && Current.Range.Resource->IsRetired())
			|| Owner->CookedProgramData.GetContentRevision() != State->BulkRevision || State->Quality != GetMaterialQualityLevel()
			|| State->FeatureLevel != PreparationFeatureLevel() || State->Backend != PreparationBackend()
			|| Current.LogicalSize != Earlier.LogicalSize || Current.Range.Resource != Earlier.Range.Resource
			|| Current.Range.SegmentOffset != Earlier.Range.SegmentOffset || Current.Range.StoredSize != Earlier.Range.StoredSize
			|| Current.Range.StorageFlags != Earlier.Range.StorageFlags || Current.Range.Alignment != Earlier.Range.Alignment)
			return State->Status = EMaterialProgramPreparationState::Stale;
		if (!State->Request.IsReady()) return State->Status;
		const auto Read = State->Request.Wait(); // Already complete; never waits for worker progress.
		if (!Read || !State->Result->Program || !State->Result->Diagnostic.empty())
		{
			if (!Read && State->Result->Diagnostic.empty()) State->Result->Diagnostic = FormatPackageResourceReadError(Read);
			Owner->MaterialCookDiagnostic = State->Result->Diagnostic;
			return State->Status = EMaterialProgramPreparationState::Failed;
		}
		const auto Admitted = Owner->AdmitCookedProgram(State->Result->Properties, State->Result->Program);
		if (!Admitted)
		{
			State->Result->Diagnostic = Admitted.error();
			Owner->MaterialCookDiagnostic = State->Result->Diagnostic;
			return State->Status = EMaterialProgramPreparationState::Failed;
		}
		Owner->MaterialCookDiagnostic.clear();
		State->Result->Program = Owner->GetAcceptedCompiledProgram();
		return State->Status = EMaterialProgramPreparationState::Ready;
	}
	auto FMaterialProgramPreparation::Cancel() const -> void { if (State && State->Status == EMaterialProgramPreparationState::Pending) State->Result->Canceled = true; }
	auto FMaterialProgramPreparation::GetProgram() const -> std::shared_ptr<const FMaterialCompilerResult>
	{
		CheckGameThread();
		return State && !State->Result->Canceled && State->Status == EMaterialProgramPreparationState::Ready ? State->Result->Program : nullptr;
	}
	auto FMaterialProgramPreparation::GetDiagnostic() const -> std::string
	{
		CheckGameThread();
		// Decode owns this string until its continuation finishes.
		return State && (State->Status == EMaterialProgramPreparationState::Failed || State->Request.IsReady())
			? State->Result->Diagnostic : std::string{};
	}
}
