#include "CookedPreparation.h"
#include "DObject/DObjectArray.h"
#include "DObject/Package.h"
#include "DObject/Class.h"
#include "Asset/AssetBuildTaskContext.h"

namespace Durin::AssetPrivate
{
	namespace { thread_local FScopedCookedPreparation* ActivePreparation = nullptr; }
	FScopedCookedPreparation::FScopedCookedPreparation() : Previous(ActivePreparation) { ActivePreparation = this; }
	FScopedCookedPreparation::~FScopedCookedPreparation()
	{
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
		uint64 RetainedBytes = 0;
		const auto Revision = Package->GetEditRevision();
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
			Current = Operation.get();
			Operations.push_back(std::move(Operation));
			Object->BeginCacheForCookedPlatformData(Context.Lifecycle);
			Current = nullptr;
		}
		// Begin all candidate operations before joining any asynchronous work.
		for (const auto& Operation : Operations) {
			auto& Context = *Operation;
			auto* Object = Context.Object;
			Current = Operation.get();
			struct FResetContext { FCookPreparationContext*& Value; ~FResetContext() { Value = nullptr; } } ResetContext{Current};
			auto Wait = std::exchange(Context.Lifecycle.WaitForCompletion, {});
			if (Wait) Wait();
			const bool Complete = Object->IsCachedCookedPlatformDataLoaded(Context.Lifecycle);
			auto Result = Context.Failure;
			if (Result && (!Context.Lifecycle.Result || !Complete))
				Result = {.Status = Context.Lifecycle.Result.Error == EObjectCookError::Cancelled ? ECookContributionStatus::Canceled : ECookContributionStatus::Error,
					.Error = ECookContributionError::Preparation, .ObjectPath = Object->GetObjectPath(), .VirtualPath = Package->GetPackagePath(),
					.Diagnostic = Complete ? Context.Lifecycle.Result.Diagnostic : "Cook platform operation did not complete after its bounded wait."};
			if (!Result) {
				Result.Provider = Object->GetClass()->GetQualifiedName().ToString();
				if (Options.PreparationFailure) *Options.PreparationFailure = Result;
				return Failure(*Object, FormatCookContributionError(Result));
			}
			FPreparedCookedObject Product;
			Product.PackageRevision = Revision;
			Product.Primary = std::move(Context.Primary);
			Product.Collision = std::move(Context.Collision);
			const auto PrimaryBytes = Product.Primary.GetMetadata().LogicalSize;
			const auto CollisionBytes = Product.Collision.GetMetadata().LogicalSize;
			constexpr uint64 MaximumBytes = 1024ull * 1024 * 1024;
			if (PrimaryBytes > MaximumBytes - RetainedBytes || CollisionBytes > MaximumBytes - RetainedBytes - PrimaryBytes)
				return Failure(*Object, "Cook prepared payload limit exceeded.");
			RetainedBytes += PrimaryBytes + CollisionBytes;
			Products.emplace(FObjectKey(Object), std::move(Product));
		}
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

	auto FScopedCookedPreparation::Find(const DObject& Object, const FArchiveTarget& Requested) -> FPreparedCookedObject*
	{
		if (!ActivePreparation || Requested.Platform != ActivePreparation->Target.Platform
			|| Requested.Profile != ActivePreparation->Target.Profile) return nullptr;
		const auto Found = ActivePreparation->Products.find(FObjectKey(&Object));
		if (Found == ActivePreparation->Products.end() || !Object.GetPackage()
			|| Object.GetPackage()->GetEditRevision() != Found->second.PackageRevision) return nullptr;
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
	auto FCookPreparationContext::Get(DObject& Object, const FObjectCookContext& Invocation) -> FCookPreparationContext*
	{
		return AssetPrivate::FScopedCookedPreparation::GetContext(Object, Invocation);
	}
}
