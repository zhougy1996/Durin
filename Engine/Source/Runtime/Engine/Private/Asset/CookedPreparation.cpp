#include "CookedPreparation.h"
#include "DObject/DObjectArray.h"
#include "DObject/Package.h"
#include "Asset/AssetBuildTaskContext.h"

namespace Durin::AssetPrivate
{
	namespace { thread_local FScopedCookedPreparation* ActivePreparation = nullptr; }
	FScopedCookedPreparation::FScopedCookedPreparation() : Previous(ActivePreparation) { ActivePreparation = this; }
	FScopedCookedPreparation::~FScopedCookedPreparation() { ActivePreparation = Previous; }

	auto FScopedCookedPreparation::Prepare(DPackage* Package, const FAssetPackageSerializationOptions& Options)
		-> std::expected<void, FPackageCaptureError>
	{
		if (Options.Domain != EAssetPackageSaveDomain::Cooked || !Package) return {};
		if (!Options.CookSnapshot) Builtins = std::make_unique<FEngineCookRegistrationScope>();
		Snapshot = Options.CookSnapshot ? Options.CookSnapshot : CaptureCookRegistry();
		Target = {std::string(GetTargetPlatformName(Options.TargetPlatform)), std::string(GetTargetProfileName(Options.TargetProfile))};
		auto Failure = [](const DObject& Object, std::string Message) -> std::expected<void, FPackageCaptureError> {
			return std::unexpected(FPackageCaptureError{.Reason = EPackageCaptureReason::ArchiveFailure,
				.ObjectPath = Object.GetObjectPath(), .ArchiveCode = EArchiveFailureCode::InvalidData, .Message = std::move(Message)});
		};
		if (Target.Platform.empty() || Options.TargetProfile != ECookTargetProfile::Game)
			return Failure(*Package, "Cook preparation requires a known Game target.");
		if (Builtins && !Builtins->Result) return Failure(*Package, FormatCookContributorRegistrationError(Builtins->Result));
		if (std::ranges::none_of(Package->GetTopLevelAssets(), [&](auto* Object) { return Snapshot->Includes(Object) && !IsPackageCaptureObjectOmitted(Object, Options.SaveOverrides.get()); }))
		{
			if (Options.PreparationFailure) *Options.PreparationFailure = {.Error = ECookContributionError::EmptyRuntimeRoot, .VirtualPath = Package->GetPackagePath()};
			return Failure(*Package, "Cook root has no runtime exports.");
		}
		const FAssetBuildTaskContext Control{.Cancellation = FCancellationToken([Check = Options.IsCancelled] { return Check && Check(); })};
		std::vector<DObject*> Pending(Package->GetTopLevelAssets().begin(), Package->GetTopLevelAssets().end());
		uint64 RetainedBytes = 0;
		const auto Revision = Package->GetEditRevision();
		for (size_t Index = 0; Index < Pending.size(); ++Index)
		{
			if (Index >= 65536) return Failure(*Package, "Cook preparation object limit exceeded.");
			if (Control.IsCancelled()) return Failure(*Package, "Cook preparation was cancelled.");
			auto* Object = Pending[Index];
			if (!Snapshot->Includes(Object) || IsPackageCaptureObjectOmitted(Object, Options.SaveOverrides.get())) continue;
			auto Children = GDObjectArray.GetObjectsWithOuter(Object,
				Package->IsGraphPrivate() ? EObjectQueryScope::IncludeUnpublished : EObjectQueryScope::LiveOnly);
			for (auto* Child : Children) if (!Child->IsTemplateObject()) Pending.push_back(Child);
			if (Pending.size() > 65536) return Failure(*Object, "Cook preparation object limit exceeded.");
			const auto* Family = Snapshot->Resolve(Object->GetClass(), Object->GetOuter() == Package);
			if (!Family || !Family->Prepare) continue;
			FCookPreparationContext Context{Options};
			auto Result = Family->Prepare(*Object, Package->GetPackagePath(), Context);
			if (!Result)
			{
				Result.Provider = Family->Name;
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
		if (Control.IsCancelled()) return Failure(*Package, "Cook preparation was cancelled.");
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
}
