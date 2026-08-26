#include "AssetForge/ImportService.h"
#include "ComponentRegistryInternal.h"
#include "ImportServicePrivate.h"
#include "AssetForge/Operations/ImportExecution.h"

namespace Durin::AssetForge
{
	using Private::FComponentRegistryRegistration;
	using Private::FComponentRegistryStore;

	FComponentRegistration::FComponentRegistration(
		FImportService& InOwner,
		std::weak_ptr<void> InOwnerLifetime,
		EComponentRole InRole,
		std::string InId,
		uint64 InIdentity)
		: Owner(&InOwner), OwnerLifetime(std::move(InOwnerLifetime)),
		Role(InRole), Id(std::move(InId)), Identity(InIdentity) {}

	FComponentRegistration::~FComponentRegistration()
	{
		Reset();
	}

	FComponentRegistration::FComponentRegistration(
		FComponentRegistration&& Other) noexcept
		: Owner(std::exchange(Other.Owner, nullptr)),
		OwnerLifetime(std::move(Other.OwnerLifetime)), Role(Other.Role),
		Id(std::move(Other.Id)), Identity(std::exchange(Other.Identity, 0)) {}

	auto FComponentRegistration::operator=(
		FComponentRegistration&& Other) noexcept -> FComponentRegistration&
	{
		if (this == &Other) return *this;
		Reset();
		Owner = std::exchange(Other.Owner, nullptr);
		OwnerLifetime = std::move(Other.OwnerLifetime);
		Role = Other.Role;
		Id = std::move(Other.Id);
		Identity = std::exchange(Other.Identity, 0);
		return *this;
	}

	auto FComponentRegistration::Reset() -> bool
	{
		if (!Owner) return false;
		const std::shared_ptr<void> LifetimeGuard = OwnerLifetime.lock();
		if (!LifetimeGuard)
		{
			Owner = nullptr;
			Id.clear();
			Identity = 0;
			return false;
		}
		FImportService* Service = std::exchange(Owner, nullptr);
		const uint64 RegistrationIdentity = std::exchange(Identity, 0);
		const std::string RegistrationId = std::move(Id);
		return Service->UnregisterComponent(
			Role, RegistrationId, RegistrationIdentity);
	}

	FImportService::FImportService()
		: Lifetime(std::make_shared<uint8>(0)), Impl(std::make_unique<FImpl>()) {}

	FImportService::~FImportService()
	{
		CloseAsyncAdmission();
		CancelAndDrainAllAsyncImports();
		Lifetime.reset();
	}

	auto FImportService::RegisterSourceTranslatorScoped(
		FSourceTranslatorRegistrationDescriptor Descriptor,
		FModuleOwnedCallbackGate OwnerGate,
		std::string& OutError) -> FComponentRegistration
	{
		const FComponentRegistryRegistration Registered =
			Impl->Import.Register(std::move(Descriptor), std::move(OwnerGate), OutError);
		if (Registered.Identity != 0) OpenAsyncImporterAdmission(Registered.Id);
		return Registered.Identity == 0 ? FComponentRegistration{}
			: FComponentRegistration(*this, Lifetime, Registered.Role,
				Registered.Id, Registered.Identity);
	}

	auto FImportService::RegisterPlanningPassScoped(
		FPlanningPassRegistrationDescriptor Descriptor,
		FModuleOwnedCallbackGate OwnerGate,
		std::string& OutError) -> FComponentRegistration
	{
		const FComponentRegistryRegistration Registered =
			Impl->Import.Register(std::move(Descriptor), std::move(OwnerGate), OutError);
		if (Registered.Identity != 0) OpenAsyncImporterAdmission(Registered.Id);
		return Registered.Identity == 0 ? FComponentRegistration{}
			: FComponentRegistration(*this, Lifetime, Registered.Role,
				Registered.Id, Registered.Identity);
	}

	auto FImportService::RegisterAssetBuilderScoped(
		FAssetBuilderRegistrationDescriptor Descriptor,
		FModuleOwnedCallbackGate OwnerGate,
		std::string& OutError) -> FComponentRegistration
	{
		const FComponentRegistryRegistration Registered =
			Impl->Import.Register(std::move(Descriptor), std::move(OwnerGate), OutError);
		if (Registered.Identity != 0) OpenAsyncImporterAdmission(Registered.Id);
		return Registered.Identity == 0 ? FComponentRegistration{}
			: FComponentRegistration(*this, Lifetime, Registered.Role,
				Registered.Id, Registered.Identity);
	}

	auto FImportService::UnregisterComponent(
		EComponentRole Role,
		std::string_view Id,
		uint64 Identity) -> bool
	{
		CancelAndDrainAsyncImportsForProvider(Id);
		return Impl->Import.Unregister(Role, Id, Identity);
	}

	auto FImportService::FindComponent(
		EComponentRole Role,
		std::string_view Id,
		uint32 ContractVersion) const -> FComponentLease
	{
		return Impl->Import.Find(Role, Id, ContractVersion);
	}

	auto FImportService::SelectSourceTranslator(
		const FImportSourceRecognition& Source,
		std::string_view PersistedId,
		uint32 PersistedVersion) const -> FComponentSelectionResult
	{
		if (!PersistedId.empty())
		{
			FComponentLease Lease = Impl->Import.Find(
				EComponentRole::Translator, PersistedId, PersistedVersion);
			if (Lease) return {.Lease = std::move(Lease)};
			return {.Diagnostics = {{
				.Category = EImportDiagnosticCategory::ProviderUnavailable,
				.Identity = "Durin.AssetForge.Diagnostic.PersistedTranslatorUnavailable",
				.Phase = "Selection",
				.Message = std::format("Persisted translator '{}' version {} is unavailable.",
					PersistedId, PersistedVersion)}}};
		}
		return Impl->Import.SelectSourceTranslator(Source);
	}

	auto FImportService::SelectAssetBuilder(
		std::string_view OutputClassName,
		std::string_view PersistedId,
		uint32 PersistedVersion) const -> FComponentSelectionResult
	{
		if (!PersistedId.empty())
		{
			FComponentLease Lease = Impl->Import.Find(
				EComponentRole::AssetBuilder, PersistedId, PersistedVersion);
			if (Lease && Lease.GetOutputClassName() == OutputClassName)
				return {.Lease = std::move(Lease)};
			return {.Diagnostics = {{
				.Category = EImportDiagnosticCategory::ProviderUnavailable,
				.Identity = "Durin.AssetForge.Diagnostic.PersistedBuilderUnavailable",
				.Phase = "Selection",
				.Message = std::format(
					"Persisted factory '{}' version {} is unavailable for output class '{}'.",
					PersistedId, PersistedVersion, OutputClassName)}}};
		}
		return Impl->Import.SelectAssetBuilder(OutputClassName);
	}

	auto FImportService::EnumerateComponents(
		EComponentRole Role) const
		-> std::vector<FComponentIdentity>
	{
		return Impl->Import.Enumerate(Role);
	}

	auto FImportService::GetComponentRevision() const -> uint64
	{
		return Impl->Import.GetRevision();
	}

	auto GetImportService() -> FImportService&
	{
		static FImportService Service;
		return Service;
	}

}
