#include "CookedPreparation.h"
#include "AssetPackageCodec.h"
#include "DObject/DObjectArray.h"
#include "DObject/Package.h"
#include "DObject/Class.h"
#include "Asset/AssetBuildTaskContext.h"
#include "Threading/Task.h"

#if DURIN_WITH_EDITOR
#include "TargetPlatform.h"
#include "DerivedDataRequestOwner.h"
#endif

namespace Durin::AssetPrivate
{
	FPackageCookPreparation::~FPackageCookPreparation()
	{
		Cancel();
		for (auto It = Objects.rbegin(); It != Objects.rend(); ++It)
			(*It)->ClearCachedCookedPlatformData(Platform);
		for (const auto& Operation : Operations) {
			Operation->TryReserveWorkingSet = {};
			Operation->bInUse = false;
		}
	}
	auto FPackageCookPreparation::Cancel() -> void
	{
		if (Result && !bComplete) Result = {EObjectCookError::Cancelled, {}};
		for (const auto& Operation : Operations) Operation->Cancel();
	}
	auto FPackageCookPreparation::Begin(DPackage* InPackage, const FCookPackagePreparationOptions& InOptions,
		const FAssetPackageSerializationOptions& InSerialization, FCookPackagePreparationReport& OutReport) -> FObjectCookResult
	{
		Package = InPackage; Options = InOptions; Serialization = InSerialization; Report = &OutReport;
#if DURIN_WITH_EDITOR
		Platform = ResolveTargetPlatform(Options.TargetPlatform);
		if (Platform) Target = {std::string(GetTargetPlatformName(Platform->GetPlatform())), std::string(GetTargetProfileName(Options.TargetProfile))};
#endif
		if (!Package || !Platform || Options.TargetProfile != ECookTargetProfile::Game)
			return Result = {EObjectCookError::InvalidInput, "Cook preparation requires a package and a known Game target."};
		std::vector<DObject*> Pending(Package->GetTopLevelAssets().begin(), Package->GetTopLevelAssets().end());
		for (size_t Index = 0; Index < Pending.size(); ++Index) {
			if (Pending.size() > 65536) return Result = {EObjectCookError::Limit, "Cook object limit exceeded."};
			if (Options.IsCancelled && Options.IsCancelled()) return Result = {EObjectCookError::Cancelled, {}};
			auto* Object = Pending[Index];
			if (!IsObjectIncludedForCook(Object, Platform) || IsPackageCaptureObjectOmitted(Object, Serialization.SaveOverrides.get())) continue;
			const auto Children = GDObjectArray.GetObjectsWithOuter(Object, Package->IsGraphPrivate() ? EObjectQueryScope::IncludeUnpublished : EObjectQueryScope::LiveOnly);
			Pending.insert(Pending.end(), Children.begin(), Children.end());
			if (auto* Existing = FCookedPlatformData::Get(*Object, Platform)) {
				if (Existing->bInUse) return Result = {EObjectCookError::Preparation, "Cook platform data is already in use."};
				if (!Existing->Result || Existing->Package != Package->GetPackagePath()
					|| Existing->Options.MaterialConfigurations != Options.MaterialConfigurations
					|| (Existing->ValidatePreparedInputs && !Existing->ValidatePreparedInputs())) Object->ClearCachedCookedPlatformData(Platform);
			}
			Objects.push_back(Object);
			Object->BeginCacheForCookedPlatformData(Platform);
			auto Operation = std::static_pointer_cast<FCookedPlatformData>(Object->GetCachedCookedPlatformData(Platform));
			if (!Operation) continue;
			Operation->bInUse = true;
			if (!Operation->HasStarted()) Operation->Options = Options;
			Operation->TryReserveWorkingSet = [this](uint64 Bytes) {
				uint64 Reserved = 0; uint32 Active = 0;
				for (const auto& Candidate : Operations) if (!Candidate->IsReady() && Candidate->WorkingSetBytes) {
					Reserved += Candidate->WorkingSetBytes; ++Active;
				}
				if (Active >= Options.MaximumConcurrentPreparation || Reserved > Options.MaximumPreparationWorkingSetBytes
					|| Bytes > Options.MaximumPreparationWorkingSetBytes - Reserved) return false;
				auto& Metrics = Report->Metrics;
				++Metrics.AdmittedOperations;
				Metrics.PeakAdmittedOperations = std::max(Metrics.PeakAdmittedOperations, Active + 1);
				Metrics.PeakReservedBytes = std::max(Metrics.PeakReservedBytes, Reserved + Bytes);
				return true;
			};
			Operations.push_back(std::move(Operation));
		}
		if (Objects.empty()) return Result = {EObjectCookError::InvalidInput, "Cook root has no runtime exports."};
		return Result;
	}
	auto FPackageCookPreparation::Poll() -> EPollStatus
	{
		if (!Result) {
			Cancel();
			bool Pending = false;
			// Keep inputs and task state alive until all canceled work has retired.
			for (const auto& Operation : Operations) if (!Operation->Poll()) Pending = true;
			return Pending ? EPollStatus::Incomplete : EPollStatus::Error;
		}
		if (bComplete) return EPollStatus::Success;
		if (Options.IsCancelled && Options.IsCancelled()) { Result = {EObjectCookError::Cancelled, {}}; return Poll(); }
		bool Pending = false;
		uint64 Retained = 0;
		for (auto* Object : Objects) {
			if (!Object->IsCachedCookedPlatformDataLoaded(Platform)) { Pending = true; continue; }
			auto* Operation = FCookedPlatformData::Get(*Object, Platform);
			if (!Operation) continue;
			if (!Operation->Result) {
				Result = Operation->Result;
				if (!Result.Diagnostic.empty()) Result.Diagnostic = Object->GetObjectPath() + ": " + Result.Diagnostic;
				return Poll();
			}
			Retained += Operation->Primary.GetMetadata().LogicalSize + Operation->Collision.GetMetadata().LogicalSize;
			if (Retained > 1024ull * 1024 * 1024) { Result = {EObjectCookError::Limit, "Cook prepared payload limit exceeded."}; return Poll(); }
		}
		Report->Metrics.RetainedProductBytes = Retained;
		if (Pending) return EPollStatus::Incomplete;
		Result = Validate(Package);
		if (!Result) return EPollStatus::Error;
		bComplete = true; return EPollStatus::Success;
	}
	auto FPackageCookPreparation::Validate(DPackage* InPackage) const -> FObjectCookResult
	{
		if (!InPackage || InPackage != Package)
			return {EObjectCookError::InvalidInput, "Cook inputs changed during preparation or save."};
		for (const auto& Operation : Operations)
			if (Operation->ValidatePreparedInputs && !Operation->ValidatePreparedInputs())
				return {EObjectCookError::InvalidInput, "Cook prepared inputs changed."};
		std::unordered_set<DObject*> Observed;
		std::vector<DObject*> Pending(InPackage->GetTopLevelAssets().begin(), InPackage->GetTopLevelAssets().end());
		for (size_t Index = 0; Index < Pending.size(); ++Index) {
			if (Pending.size() > 65536) return {EObjectCookError::Limit, "Cook object limit exceeded."};
			auto* Object = Pending[Index];
			if (!IsObjectIncludedForCook(Object, Platform) || IsPackageCaptureObjectOmitted(Object, Serialization.SaveOverrides.get())) continue;
			Observed.insert(Object);
			auto Children = GDObjectArray.GetObjectsWithOuter(Object, InPackage->IsGraphPrivate() ? EObjectQueryScope::IncludeUnpublished : EObjectQueryScope::LiveOnly);
			Pending.insert(Pending.end(), Children.begin(), Children.end());
		}
		if (Observed.size() != Objects.size() || std::ranges::any_of(Objects, [&](auto* Object) { return !Observed.contains(Object); }))
			return {EObjectCookError::InvalidInput, "Cook object graph changed during preparation or save."};
		return {};
	}
}

namespace Durin
{
	auto SerializeCookedPackageClosure(DPackage* Package, FByteBuffer& OutBytes,
		FByteBuffer& OutBulkBytes, const FCookPackagePreparationOptions& Preparation,
		FCookPackagePreparationReport* OutReport, const FAssetPackageSerializationOptions& Serialization)
		-> FObjectCookResult
	{
		FCookPackagePreparationReport LocalReport;
		auto& Report = OutReport ? *OutReport : LocalReport;
		Report = {};
		AssetPrivate::FPackageCookPreparation Prepared;
		if (auto Result = Prepared.Begin(Package, Preparation, Serialization, Report); !Result) return Result;
		// Only the synchronous serialization boundary waits; Poll never waits for budget or tasks.
		while (Prepared.Poll() == AssetPrivate::EPollStatus::Incomplete)
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		if (!Prepared.GetResult()) return Prepared.GetResult();
		AssetPrivate::FAssetPackageEncodedClosure Closure;
		const AssetPrivate::FAssetPackageCaptureOptions Capture{
			.Serialization = Serialization,
			.CookTarget = Prepared.GetArchiveTarget(),
			.TargetPlatform = Prepared.GetTargetPlatform()};
		if (auto Result = AssetPrivate::EncodeAssetPackageClosure(Package, Closure, Capture); !Result)
			return {EObjectCookError::Preparation, ObjectPackage::FormatPackageError(Result)};
		if (auto Result = Prepared.Validate(Package); !Result) return Result;
		OutBytes = std::move(Closure.PackageBytes);
		OutBulkBytes = std::move(Closure.BulkBytes);
		return {};
	}

	auto SerializeCookedPackageBytes(DPackage* Package, FByteBuffer& OutBytes,
		const FCookPackagePreparationOptions& Preparation, FCookPackagePreparationReport* Report,
		const FAssetPackageSerializationOptions& Serialization) -> FObjectCookResult
	{
		FByteBuffer Bulk;
		return SerializeCookedPackageClosure(Package, OutBytes, Bulk, Preparation, Report, Serialization);
	}

	auto FCookedPlatformData::StartDetachedWork(FCookDetachedWork Work) -> void
	{
		if (!WorkingSetBytes || PollWork) {
			Result = {EObjectCookError::InvalidInput, "Detached Cook work requires one admitted reservation."}; return;
		}
		struct FWorkState
		{
			Tasks::FCancellationToken Cancellation;
#if DURIN_WITH_EDITOR
			DerivedData::FRequestOwner RequestOwner;
#endif
			Tasks::FTask Task;
			std::mutex Mutex;
			std::expected<FCookPreparationProducts, std::string> Result = std::unexpected("Cook task did not produce a result.");
		};
		auto State = std::make_shared<FWorkState>();
		auto Run = [State, Work = std::move(Work), Bytes = WorkingSetBytes]() mutable {
			std::expected<FCookPreparationProducts, std::string> Result = std::unexpected("Cook task was cancelled.");
			try {
				if (!State->Cancellation.IsCanceled()) Result = Work({.Cancellation = &State->Cancellation,
#if DURIN_WITH_EDITOR
					.RequestOwner = &State->RequestOwner,
#endif
					.MaximumWorkingSetBytes = Bytes});
			} catch (const std::exception& Error) { Result = std::unexpected(MakeAssetBuildDiagnostic(Error.what())); }
			catch (...) { Result = std::unexpected("Cook task failed with an exception."); }
			std::lock_guard Lock(State->Mutex);
			State->Result = std::move(Result);
		};
		if (IsTaskSchedulerRunning()) {
			State->Task = Tasks::LaunchTask("CookDetachedPreparation", std::move(Run));
		} else Run();
		CancelWork = [State] {
			State->Cancellation.Cancel();
			// Accepted DDC work runs to completion; cancellation prevents result installation.
		};
		WaitWork = [State] { if (State->Task.IsValid()) (void)State->Task.Wait(); };
		PollWork = [this, State] {
			if (State->Task.IsValid() && !State->Task.IsCompleted()) return false;
			if (State->Task.IsValid()) (void)State->Task.Wait();
			std::lock_guard Lock(State->Mutex);
			if (State->Cancellation.IsCanceled()) {
				Result = {EObjectCookError::Cancelled, {}}; return true;
			}
			if (!State->Result) { Result = {EObjectCookError::Preparation, std::move(State->Result.error())}; return true; }
			Primary = std::move(State->Result->Primary);
			Collision = std::move(State->Result->Collision);
			return true;
		};
	}

	FCookedPlatformData::FCookedPlatformData(const FCookPackagePreparationOptions& InOptions) : Options(InOptions)
	{
#if DURIN_WITH_EDITOR
		Platform = ResolveTargetPlatform(Options.TargetPlatform);
#endif
	}
	FCookedPlatformData::~FCookedPlatformData() { Cancel(); Wait(); }
	auto FCookedPlatformData::Cancel() -> void
	{
		if (bReady || bCancelled) return;
		bCancelled = true;
		if (CancelWork) CancelWork();
	}
	auto FCookedPlatformData::CheckCancellation() -> bool
	{
		if (Options.IsCancelled && Options.IsCancelled()) Cancel();
		if (bCancelled && Result) Result = {EObjectCookError::Cancelled, {}};
		return bCancelled;
	}
	auto FCookedPlatformData::ReserveWorkingSet(uint64 Bytes) -> bool
	{
		if (!Bytes || Bytes > Options.MaximumPreparationWorkingSetBytes || !Options.MaximumConcurrentPreparation) {
			Result = {EObjectCookError::Limit, "Cook preparation reservation exceeds its budget."}; return false;
		}
		if (TryReserveWorkingSet && !TryReserveWorkingSet(Bytes)) { bAwaitingBudget = true; return false; }
		WorkingSetBytes = Bytes; return true;
	}
	auto FCookedPlatformData::Poll() -> bool
	{
		if (bReady) return true;
		(void)CheckCancellation();
		if (!bStarted && StartWork && Result) {
			bAwaitingBudget = false;
			StartWork(*this);
			if (bAwaitingBudget && Result) return false;
			bStarted = true; StartWork = {};
		}
		if (!Result && !bStarted) StartWork = {};
		if (PollWork && !PollWork()) return false;
		(void)CheckCancellation();
		PollWork = {}; CancelWork = {}; WaitWork = {}; TryReserveWorkingSet = {};
		WorkingSetBytes = 0; bReady = true;
		return true;
	}
	auto FCookedPlatformData::Wait() -> void
	{
		while (!Poll()) {
			// Normal synchronous callers still pump their owner-thread cancellation
			// source. Once canceled, native waits drain callbacks without polling.
			if (bCancelled && WaitWork) WaitWork();
			else std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	}
	auto FCookedPlatformData::Begin(DObject& Object, const ITargetPlatform* Target,
		std::function<void(FCookedPlatformData&)> Start) -> void
	{
		if (!Target) return;
		if (auto* Existing = Get(Object, Target)) {
			if (Existing->bInUse || ((Object.GetPackage() ? Existing->Package == Object.GetPackage()->GetPackagePath() : Existing->Package.empty())
				&& (!Existing->ValidatePreparedInputs || Existing->ValidatePreparedInputs()))) return;
			Object.ClearCachedCookedPlatformData(Target);
		}
		FCookPackagePreparationOptions Options;
#if DURIN_WITH_EDITOR
		Options.TargetPlatform = Target->GetPlatform();
#endif
		Options.TargetProfile = ECookTargetProfile::Game;
		auto Data = std::make_shared<FCookedPlatformData>(Options);
		Data->Object = &Object;
		if (Object.GetPackage()) Data->Package = Object.GetPackage()->GetPackagePath();
		Data->StartWork = std::move(Start);
		Object.SetCookedPlatformData(Target, std::move(Data));
	}
	auto FCookedPlatformData::Find(const DObject& Object, const FArchiveTarget& Target) -> FCookedPlatformData*
	{
#if DURIN_WITH_EDITOR
		const auto Platform = ParseTargetPlatform(Target.Platform);
		if (!Platform) return nullptr;
		auto* Data = Get(Object, ResolveTargetPlatform(*Platform));
		if (!Data || !Data->bReady || !Data->Result || Target.Profile != "Game"
			|| !Object.GetPackage() || Object.GetPackage()->GetPackagePath() != Data->Package
			|| (Data->ValidatePreparedInputs && !Data->ValidatePreparedInputs())) return nullptr;
		return Data;
#else
		return nullptr;
#endif
	}
	auto FCookedPlatformData::Get(const DObject& Object, const ITargetPlatform* Target) -> FCookedPlatformData*
	{
		return static_cast<FCookedPlatformData*>(Object.GetCachedCookedPlatformData(Target).get());
	}
}
