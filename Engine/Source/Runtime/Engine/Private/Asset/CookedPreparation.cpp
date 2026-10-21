#include "CookedPreparation.h"
#include "DObject/DObjectArray.h"
#include "DObject/Package.h"
#include "DObject/Class.h"
#include "Asset/AssetBuildTaskContext.h"
#include "Threading/Task.h"

namespace Durin::AssetPrivate
{
	namespace { thread_local FScopedCookedPreparation* ActivePreparation = nullptr; }
	FScopedCookedPreparation::FScopedCookedPreparation() : Previous(ActivePreparation) { ActivePreparation = this; }
	FScopedCookedPreparation::~FScopedCookedPreparation()
	{
		for (const auto& Operation : Operations) if (Operation->CancelWork) Operation->CancelWork();
		for (auto It = Operations.rbegin(); It != Operations.rend(); ++It) {
			Current = It->get();
			auto Wait = std::exchange(Current->Lifecycle.WaitForCompletion, {});
			if (Wait) Wait();
			Current->Object->ClearCachedCookedPlatformData(Current->Lifecycle);
		}
		Current = nullptr;
		ActivePreparation = Previous;
	}

	auto FScopedCookedPreparation::Prepare(DPackage* Package, const FAssetPackageSerializationOptions& Options)
		-> std::expected<void, FPackageCaptureError>
	{
		if (Options.Domain != EAssetPackageSaveDomain::Cooked || !Package) return {};
		if (Options.PreparationMetrics) *Options.PreparationMetrics = {};
		Target = {std::string(GetTargetPlatformName(Options.TargetPlatform)), std::string(GetTargetProfileName(Options.TargetProfile))};
		auto Failure = [](const DObject& Object, std::string Message) -> std::expected<void, FPackageCaptureError> {
			return std::unexpected(FPackageCaptureError{.Reason = EPackageCaptureReason::ArchiveFailure,
				.ObjectPath = Object.GetObjectPath(), .ArchiveCode = EArchiveFailureCode::InvalidData, .Message = std::move(Message)});
		};
		if (Target.Platform.empty() || Options.TargetProfile != ECookTargetProfile::Game)
			return Failure(*Package, "Cook preparation requires a known Game target.");
		if (std::ranges::none_of(Package->GetTopLevelAssets(), [&](auto* Object) { return IsObjectIncludedForCook(Object, Target) && !IsPackageCaptureObjectOmitted(Object, Options.SaveOverrides.get()); }))
		{
			if (Options.PreparationFailure) *Options.PreparationFailure = {.Status = ECookContributionStatus::Error, .Error = ECookContributionError::EmptyRuntimeRoot, .VirtualPath = Package->GetPackagePath()};
			return Failure(*Package, "Cook root has no runtime exports.");
		}
		const FAssetBuildTaskContext Control{.Cancellation = FCancellationToken([Check = Options.IsCancelled] { return Check && Check(); })};
		std::vector<DObject*> Pending(Package->GetTopLevelAssets().begin(), Package->GetTopLevelAssets().end());
		Revision = Package->GetEditRevision();
		for (size_t Index = 0; Index < Pending.size(); ++Index)
		{
			if (Index >= 65536) return Failure(*Package, "Cook preparation object limit exceeded.");
			if (Control.IsCancelled())
			{
				if (Options.PreparationFailure) *Options.PreparationFailure = {.Status = ECookContributionStatus::Canceled};
				return Failure(*Package, {});
			}
			auto* Object = Pending[Index];
			if (!IsObjectIncludedForCook(Object, Target) || IsPackageCaptureObjectOmitted(Object, Options.SaveOverrides.get())) continue;
			auto Children = GDObjectArray.GetObjectsWithOuter(Object,
				Package->IsGraphPrivate() ? EObjectQueryScope::IncludeUnpublished : EObjectQueryScope::LiveOnly);
			for (auto* Child : Children) if (!Child->IsTemplateObject()) Pending.push_back(Child);
			if (Pending.size() > 65536) return Failure(*Object, "Cook preparation object limit exceeded.");
			auto Operation = std::make_unique<FCookPreparationContext>(Options);
			auto& Context = *Operation;
			Context.Object = Object;
			Context.Lifecycle.Target = Target;
			Context.Lifecycle.Package = Package->GetPackagePath();
			Context.Lifecycle.bCooking = true;
			Context.Lifecycle.bRetainEditorOnlyData = Options.bRetainEditorOnlyData;
			Context.Lifecycle.IsCancelled = Options.IsCancelled;
			Context.ReserveWorkingSet = [this, &Context, &Options](uint64 Bytes) {
				if (Context.WorkingSetBytes || !Bytes || Bytes > Options.MaximumPreparationWorkingSetBytes
					|| !Options.MaximumConcurrentPreparation) {
					Context.Lifecycle.Result = {EObjectCookError::Limit, "Cook preparation reservation exceeds its budget."}; return false;
				}
				for (;;) {
					uint64 Reserved = 0; uint32 Active = 0;
					FCookPreparationContext* Earlier = nullptr;
					for (const auto& Candidate : Operations) if (Candidate->WorkingSetBytes && !Candidate->bAdopted) {
						Reserved += Candidate->WorkingSetBytes; ++Active;
						if (Candidate.get() != &Context && !Earlier) Earlier = Candidate.get();
					}
					if (Active < Options.MaximumConcurrentPreparation && Bytes <= Options.MaximumPreparationWorkingSetBytes - Reserved) {
						if (auto* Metrics = Options.PreparationMetrics) {
							++Metrics->AdmittedOperations;
							Metrics->PeakAdmittedOperations = std::max(Metrics->PeakAdmittedOperations, Active + 1);
							Metrics->PeakReservedBytes = std::max(Metrics->PeakReservedBytes, Reserved + Bytes);
						}
						break;
					}
					if (!Earlier) { Context.Lifecycle.Result = {EObjectCookError::Limit, "Cook preparation cannot admit an operation."}; return false; }
					if (auto Result = Finish(*Earlier); !Result) {
						Context.Lifecycle.Result = {EObjectCookError::Preparation, Result.error().Message}; return false;
					}
				}
				Context.WorkingSetBytes = Bytes; return true;
			};
			Current = Operation.get();
			Operations.push_back(std::move(Operation));
			Object->BeginCacheForCookedPlatformData(Context.Lifecycle);
			Current = nullptr;
		}
		// Begin independent work before joining, except when admission must drain it.
		for (const auto& Operation : Operations)
			if (auto Result = Finish(*Operation); !Result) return Result;
		std::unordered_set<DObject*> PreparedObjects;
		for (const auto& Operation : Operations) PreparedObjects.insert(Operation->Object);
		std::vector<DObject*> Observed(Package->GetTopLevelAssets().begin(), Package->GetTopLevelAssets().end());
		uint64 ObservedCount = 0;
		for (size_t Index = 0; Index < Observed.size(); ++Index)
		{
			if (Observed.size() > 65536) return Failure(*Package, "Cook preparation object limit exceeded.");
			auto* Object = Observed[Index];
			if (!IsObjectIncludedForCook(Object, Target) || IsPackageCaptureObjectOmitted(Object, Options.SaveOverrides.get())) continue;
			if (!PreparedObjects.contains(Object))
				return std::unexpected(FPackageCaptureError{.Reason = EPackageCaptureReason::DiscoveryMutation, .ObjectPath = Object->GetObjectPath()});
			++ObservedCount;
			const auto Children = GDObjectArray.GetObjectsWithOuter(Object,
				Package->IsGraphPrivate() ? EObjectQueryScope::IncludeUnpublished : EObjectQueryScope::LiveOnly);
			Observed.insert(Observed.end(), Children.begin(), Children.end());
		}
		if (ObservedCount != PreparedObjects.size())
			return std::unexpected(FPackageCaptureError{.Reason = EPackageCaptureReason::DiscoveryMutation});
		if (Control.IsCancelled())
		{
			if (Options.PreparationFailure) *Options.PreparationFailure = {.Status = ECookContributionStatus::Canceled};
			return Failure(*Package, {});
		}
		if (Package->GetEditRevision() != Revision) return Failure(*Package, "Cook inputs changed during preparation.");
		return {};
	}

	auto FScopedCookedPreparation::Finish(FCookPreparationContext& Context) -> std::expected<void, FPackageCaptureError>
	{
		if (Context.bAdopted) return {};
		auto* Object = Context.Object;
		auto* PreviousContext = Current;
		Current = &Context;
		struct FResetContext { FCookPreparationContext*& Value; FCookPreparationContext* Previous; ~FResetContext() { Value = Previous; } } ResetContext{Current, PreviousContext};
		auto Wait = std::exchange(Context.Lifecycle.WaitForCompletion, {});
		if (Wait) Wait();
		const bool Complete = Object->IsCachedCookedPlatformDataLoaded(Context.Lifecycle);
		if (Context.ValidatePreparedInputs && !Context.ValidatePreparedInputs())
			Context.Lifecycle.Result = {EObjectCookError::InvalidInput, "Cook prepared inputs changed."};
		auto Result = Context.Failure;
		if (Result && (!Context.Lifecycle.Result || !Complete))
			Result = {.Status = Context.Lifecycle.Result.Error == EObjectCookError::Cancelled ? ECookContributionStatus::Canceled : ECookContributionStatus::Error,
				.Error = ECookContributionError::Preparation, .ObjectPath = Object->GetObjectPath(), .VirtualPath = Context.Lifecycle.Package,
				.Diagnostic = Complete ? Context.Lifecycle.Result.Diagnostic : "Cook platform operation did not complete after its bounded wait."};
		if (!Result) {
			Result.Provider = Object->GetClass()->GetQualifiedName().ToString();
			if (Context.Options.PreparationFailure) *Context.Options.PreparationFailure = Result;
			return std::unexpected(FPackageCaptureError{.Reason = EPackageCaptureReason::ArchiveFailure, .ObjectPath = Object->GetObjectPath(), .ArchiveCode = EArchiveFailureCode::InvalidData, .Message = FormatCookContributionError(Result)});
		}
		FPreparedCookedObject Product;
		Product.PackageRevision = Revision;
		Product.ValidateInputs = std::move(Context.ValidatePreparedInputs);
		Product.Primary = std::move(Context.Primary);
		Product.Collision = std::move(Context.Collision);
		const auto PrimaryBytes = Product.Primary.GetMetadata().LogicalSize;
		const auto CollisionBytes = Product.Collision.GetMetadata().LogicalSize;
		constexpr uint64 MaximumBytes = 1024ull * 1024 * 1024;
		if (PrimaryBytes > MaximumBytes - RetainedBytes || CollisionBytes > MaximumBytes - RetainedBytes - PrimaryBytes)
			return std::unexpected(FPackageCaptureError{.Reason = EPackageCaptureReason::ArchiveFailure, .Message = "Cook prepared payload limit exceeded."});
		RetainedBytes += PrimaryBytes + CollisionBytes;
		if (Context.Options.PreparationMetrics) Context.Options.PreparationMetrics->RetainedProductBytes = RetainedBytes;
		Products.emplace(FObjectKey(Object), std::move(Product));
		Context.WorkingSetBytes = 0;
		Context.CancelWork = {};
		Context.bAdopted = true;
		return {};
	}

	auto FScopedCookedPreparation::Validate(DPackage* Package) const -> std::expected<void, FPackageCaptureError>
	{
		if (Target.Platform.empty()) return {};
		if (!Package || Package->GetEditRevision() != Revision
			|| std::ranges::any_of(Products, [](const auto& Pair) { return Pair.second.ValidateInputs && !Pair.second.ValidateInputs(); }))
			return std::unexpected(FPackageCaptureError{.Reason = EPackageCaptureReason::DiscoveryMutation, .Message = "Cook inputs changed during save."});
		return {};
	}

	auto FScopedCookedPreparation::Find(const DObject& Object, const FArchiveTarget& Requested) -> FPreparedCookedObject*
	{
		if (!ActivePreparation || Requested.Platform != ActivePreparation->Target.Platform
			|| Requested.Profile != ActivePreparation->Target.Profile) return nullptr;
		const auto Found = ActivePreparation->Products.find(FObjectKey(&Object));
		if (Found == ActivePreparation->Products.end() || !Object.GetPackage()
			|| Object.GetPackage()->GetEditRevision() != Found->second.PackageRevision
			|| (Found->second.ValidateInputs && !Found->second.ValidateInputs())) return nullptr;
		return &Found->second;
	}

	auto FScopedCookedPreparation::GetContext(DObject& Object, const FObjectCookContext& Invocation) -> FCookPreparationContext*
	{
		auto* Context = ActivePreparation ? ActivePreparation->Current : nullptr;
		return Context && Context->Object == &Object && &Context->Lifecycle == &Invocation ? Context : nullptr;
	}

}

namespace Durin
{
	auto FCookPreparationContext::StartDetachedWork(FCookDetachedWork Work) -> void
	{
		if (!WorkingSetBytes || Lifecycle.WaitForCompletion) {
			Lifecycle.Result = {EObjectCookError::InvalidInput, "Detached Cook work requires one admitted reservation."}; return;
		}
		struct FWorkState
		{
			FCancellationToken Cancellation;
			Tasks::FTask Task;
			std::mutex Mutex;
			std::expected<FCookPreparationProducts, std::string> Result = std::unexpected("Cook task did not produce a result.");
		};
		auto State = std::make_shared<FWorkState>();
		auto Run = [State, Work = std::move(Work), Bytes = WorkingSetBytes](const FTaskCancellationToken&) mutable {
			std::expected<FCookPreparationProducts, std::string> Result = std::unexpected("Cook task was cancelled.");
			try {
				if (!State->Cancellation.IsCancelled()) Result = Work({.Cancellation = State->Cancellation, .MaximumWorkingSetBytes = Bytes});
			} catch (const std::exception& Error) { Result = std::unexpected(MakeAssetBuildDiagnostic(Error.what())); }
			catch (...) { Result = std::unexpected("Cook task failed with an exception."); }
			std::lock_guard Lock(State->Mutex);
			State->Result = std::move(Result);
		};
		if (IsTaskSchedulerRunning()) {
			auto Admission = Private::TryLaunchCancelableTaskWithCompletion("CookDetachedPreparation", std::move(Run), [](ETaskState) {}, {.EstimatedPayloadBytes = WorkingSetBytes});
			if (!Admission.HasValue()) { Lifecycle.Result = {EObjectCookError::Preparation, "Cook task admission failed."}; return; }
			State->Task = std::move(Admission).TakeValue();
		} else Run({});
		CancelWork = [State] { State->Cancellation.Cancel(); };
		Lifecycle.WaitForCompletion = [this, State] {
			if (State->Task.IsValid()) {
				while (!State->Task.IsCompleted()) {
					if (Lifecycle.CheckCancellation()) State->Cancellation.Cancel();
					(void)WaitTask(State->Task, 0.01);
				}
				(void)WaitTask(State->Task);
			}
			std::lock_guard Lock(State->Mutex);
			if (Lifecycle.CheckCancellation() || State->Cancellation.IsCancelled()) {
				Lifecycle.Result = {EObjectCookError::Cancelled, {}}; return;
			}
			if (!State->Result) { Lifecycle.Result = {EObjectCookError::Preparation, std::move(State->Result.error())}; return; }
			Primary = std::move(State->Result->Primary);
			Collision = std::move(State->Result->Collision);
		};
	}

	auto FCookPreparationContext::Get(DObject& Object, const FObjectCookContext& Invocation) -> FCookPreparationContext*
	{
		return AssetPrivate::FScopedCookedPreparation::GetContext(Object, Invocation);
	}
}
