#include "ImportService.h"
#include "ImportRegistryInternal.h"
#include "InterchangeRegistryInternal.h"
#include "InterchangeJob.h"

namespace Durin::Asset
{
	FInterchangeRegistration::FInterchangeRegistration(
		FImportService& InOwner,
		std::weak_ptr<void> InOwnerLifetime,
		EInterchangeComponentRole InRole,
		std::string InId,
		uint64 InIdentity)
		: Owner(&InOwner), OwnerLifetime(std::move(InOwnerLifetime)),
		Role(InRole), Id(std::move(InId)), Identity(InIdentity) {}

	FInterchangeRegistration::~FInterchangeRegistration()
	{
		Reset();
	}

	FInterchangeRegistration::FInterchangeRegistration(
		FInterchangeRegistration&& Other) noexcept
		: Owner(std::exchange(Other.Owner, nullptr)),
		OwnerLifetime(std::move(Other.OwnerLifetime)), Role(Other.Role),
		Id(std::move(Other.Id)), Identity(std::exchange(Other.Identity, 0)) {}

	auto FInterchangeRegistration::operator=(
		FInterchangeRegistration&& Other) noexcept -> FInterchangeRegistration&
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

	auto FInterchangeRegistration::Reset() -> bool
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
		return Service->UnregisterInterchangeComponent(
			Role, RegistrationId, RegistrationIdentity);
	}

FImporterRegistration::FImporterRegistration(FImportService& InOwner,
		std::weak_ptr<void> InOwnerLifetime, std::string InProviderId, uint64 InIdentity)
		: Owner(&InOwner), OwnerLifetime(std::move(InOwnerLifetime)),
		ProviderId(std::move(InProviderId)), Identity(InIdentity) {}

	FImporterRegistration::~FImporterRegistration()
	{
		Reset();
	}

	FImporterRegistration::FImporterRegistration(FImporterRegistration&& Other) noexcept
		: Owner(std::exchange(Other.Owner, nullptr)),
		OwnerLifetime(std::move(Other.OwnerLifetime)),
		ProviderId(std::move(Other.ProviderId)), Identity(std::exchange(Other.Identity, 0)) {}

	auto FImporterRegistration::operator=(FImporterRegistration&& Other) noexcept
		-> FImporterRegistration&
	{
		if (this == &Other) return *this;
		Reset();
		Owner = std::exchange(Other.Owner, nullptr);
		OwnerLifetime = std::move(Other.OwnerLifetime);
		ProviderId = std::move(Other.ProviderId);
		Identity = std::exchange(Other.Identity, 0);
		return *this;
	}

	auto FImporterRegistration::Reset() -> bool
	{
		if (!Owner) return false;
		const std::shared_ptr<void> LifetimeGuard = OwnerLifetime.lock();
		if (!LifetimeGuard)
		{
			Owner = nullptr;
			ProviderId.clear();
			Identity = 0;
			return false;
		}
		FImportService* Service = std::exchange(Owner, nullptr);
		const uint64 RegistrationIdentity = std::exchange(Identity, 0);
		const std::string RegistrationProviderId = std::move(ProviderId);
		return Service->UnregisterImporter(RegistrationProviderId, RegistrationIdentity);
	}

	namespace
	{
		class FDeclarativeImportProvider final : public IImportProvider
		{
		public:
			FDeclarativeImportProvider(std::string InId, uint32 InVersion,
				std::vector<std::string> InExtensions)
				: Id(std::move(InId)), Version(InVersion), Extensions(std::move(InExtensions))
			{
				for (std::string& Extension : Extensions)
					std::ranges::transform(Extension, Extension.begin(), [](unsigned char Character) {
						return static_cast<char>(std::tolower(Character));
					});
			}
			auto GetProviderId() const -> std::string_view override { return Id; }
			auto GetContractVersion() const -> uint32 override { return Version; }
			auto CanImport(const FImportSourceRecognition& Source) const -> bool override
			{
				std::string Extension = Source.Extension;
				std::ranges::transform(Extension, Extension.begin(), [](unsigned char Character) {
					return static_cast<char>(std::tolower(Character));
				});
				return std::ranges::find(Extensions, Extension) != Extensions.end();
			}
			auto CaptureSettings(FImportPayload& OutSettings,
				std::vector<FImportDiagnostic>&) const -> bool override
			{
				OutSettings.SchemaId = std::format("Durin.{}.DefaultSettings", Id);
				OutSettings.SchemaVersion = 1;
				OutSettings.Bytes.clear();
				return true;
			}
			auto DiscoverDependencies(std::span<const FSourceSnapshotEntry>,
				FDependencyRequestSink&, std::vector<FImportDiagnostic>&) const -> bool override
			{
				return true;
			}
			auto Plan(const FSourceSnapshot&, const FImportPayload&, FImportPlanBuilder&,
				std::vector<FImportDiagnostic>&) const -> bool override { return true; }
		private:
			std::string Id;
			uint32 Version = 0;
			std::vector<std::string> Extensions;
		};
	}

	struct FImportService::FImpl
	{
		struct FRegistration
		{
			uint64 Identity = 0;
		};

		mutable std::mutex Mutex;
		FImporterStore Providers;
		FInterchangeRegistryStore Interchange;
		std::map<std::string, FRegistration, std::less<>> Registrations;
		uint64 Revision = 1;
		uint64 NextRegistrationIdentity = 1;
	};

	FImportService::FImportService()
		: Lifetime(std::make_shared<uint8>(0)), Impl(std::make_unique<FImpl>()) {}

	FImportService::~FImportService()
	{
		Lifetime.reset();
	}

	auto FImportService::RegisterImporter(FImporterDescriptor Descriptor,
		FModuleOwnedCallbackGate OwnerGate, std::string& OutError) -> bool
	{
		FImporterRegistration Registration = RegisterImporterScoped(
			std::move(Descriptor), OwnerGate, OutError);
		if (!Registration) return false;
		Registration.Owner = nullptr;
		Registration.OwnerLifetime.reset();
		Registration.ProviderId.clear();
		Registration.Identity = 0;
		return true;
	}

	auto FImportService::RegisterImporterScoped(FImporterDescriptor Descriptor,
		FModuleOwnedCallbackGate OwnerGate, std::string& OutError)
		-> FImporterRegistration
	{
		if (!Descriptor.Provider)
		{
			if (Descriptor.ProviderId.empty() || Descriptor.ContractVersion == 0
				|| Descriptor.SourceExtensions.empty())
			{
				OutError = "An importer descriptor requires a provider or declarative source identity.";
				return {};
			}
			Descriptor.Provider = std::make_shared<FDeclarativeImportProvider>(
				std::move(Descriptor.ProviderId), Descriptor.ContractVersion,
				std::move(Descriptor.SourceExtensions));
		}
		const std::string ProviderId(Descriptor.Provider->GetProviderId());
		if (ProviderId.empty())
		{
			OutError = "An importer descriptor requires a non-empty provider id.";
			return {};
		}
		std::lock_guard Lock(Impl->Mutex);
		if (Impl->Registrations.contains(ProviderId))
		{
			OutError = "An importer descriptor is already registered for provider '"
				+ ProviderId + "'.";
			return {};
		}
		if (!Impl->Providers.Register(std::move(Descriptor.Provider), OwnerGate, OutError))
			return {};

		FImpl::FRegistration Registration;
		Registration.Identity = Impl->NextRegistrationIdentity++;
		const uint64 Identity = Registration.Identity;
		Impl->Registrations.emplace(ProviderId, std::move(Registration));
		++Impl->Revision;
		OpenAsyncImporterAdmission(ProviderId);
		OutError.clear();
		return FImporterRegistration(*this, Lifetime, ProviderId, Identity);
	}

	auto FImportService::UnregisterImporter(std::string_view ProviderId) -> bool
	{
		std::lock_guard Lock(Impl->Mutex);
		const auto It = Impl->Registrations.find(ProviderId);
		if (It == Impl->Registrations.end()) return false;
		CancelAndDrainAsyncImportsForProvider(ProviderId);
		Impl->Providers.Unregister(ProviderId);
		Impl->Registrations.erase(It);
		++Impl->Revision;
		return true;
	}

	auto FImportService::UnregisterImporter(
		std::string_view ProviderId, uint64 Identity) -> bool
	{
		std::lock_guard Lock(Impl->Mutex);
		const auto It = Impl->Registrations.find(ProviderId);
		if (It == Impl->Registrations.end() || It->second.Identity != Identity)
			return false;
		CancelAndDrainAsyncImportsForProvider(ProviderId);
		Impl->Providers.Unregister(ProviderId);
		Impl->Registrations.erase(It);
		++Impl->Revision;
		return true;
	}

	auto FImportService::GetRevision() const -> uint64
	{
		std::lock_guard Lock(Impl->Mutex);
		return Impl->Revision;
	}

	auto FImportService::FindProvider(std::string_view ProviderId) const -> FProviderLease
	{
		return Impl->Providers.Find(ProviderId);
	}
	auto FImportService::IsImporterRegistered(std::string_view ProviderId) const -> bool
	{
		std::lock_guard Lock(Impl->Mutex);
		return Impl->Registrations.contains(ProviderId);
	}

	auto FImportService::FindImporter(std::string_view ProviderId) const -> FProviderLease
	{
		return FindProvider(ProviderId);
	}

	auto FImportService::GetOutstandingImporterLeaseCount(
		std::string_view ProviderId) const -> uint64
	{
		return Impl->Providers.GetOutstandingLeaseCount(ProviderId);
	}

	auto FImportService::GetImporterRevision() const -> uint64
	{
		return Impl->Providers.GetRevision();
	}

	auto FImportService::RegisterTranslatorScoped(
		FTranslatorRegistrationDescriptor Descriptor,
		FModuleOwnedCallbackGate OwnerGate,
		std::string& OutError) -> FInterchangeRegistration
	{
		const FInterchangeRegistryRegistration Registered =
			Impl->Interchange.Register(std::move(Descriptor), std::move(OwnerGate), OutError);
		if (Registered.Identity != 0) OpenAsyncImporterAdmission(Registered.Id);
		return Registered.Identity == 0 ? FInterchangeRegistration{}
			: FInterchangeRegistration(*this, Lifetime, Registered.Role,
				Registered.Id, Registered.Identity);
	}

	auto FImportService::RegisterPipelineScoped(
		FPipelineRegistrationDescriptor Descriptor,
		FModuleOwnedCallbackGate OwnerGate,
		std::string& OutError) -> FInterchangeRegistration
	{
		const FInterchangeRegistryRegistration Registered =
			Impl->Interchange.Register(std::move(Descriptor), std::move(OwnerGate), OutError);
		if (Registered.Identity != 0) OpenAsyncImporterAdmission(Registered.Id);
		return Registered.Identity == 0 ? FInterchangeRegistration{}
			: FInterchangeRegistration(*this, Lifetime, Registered.Role,
				Registered.Id, Registered.Identity);
	}

	auto FImportService::RegisterFactoryScoped(
		FFactoryRegistrationDescriptor Descriptor,
		FModuleOwnedCallbackGate OwnerGate,
		std::string& OutError) -> FInterchangeRegistration
	{
		const FInterchangeRegistryRegistration Registered =
			Impl->Interchange.Register(std::move(Descriptor), std::move(OwnerGate), OutError);
		if (Registered.Identity != 0) OpenAsyncImporterAdmission(Registered.Id);
		return Registered.Identity == 0 ? FInterchangeRegistration{}
			: FInterchangeRegistration(*this, Lifetime, Registered.Role,
				Registered.Id, Registered.Identity);
	}

	auto FImportService::UnregisterInterchangeComponent(
		EInterchangeComponentRole Role,
		std::string_view Id,
		uint64 Identity) -> bool
	{
		ClearInterchangePreviewCache();
		CancelAndDrainAsyncImportsForProvider(Id);
		return Impl->Interchange.Unregister(Role, Id, Identity);
	}

	auto FImportService::FindInterchangeComponent(
		EInterchangeComponentRole Role,
		std::string_view Id,
		uint32 ContractVersion) const -> FInterchangeComponentLease
	{
		return Impl->Interchange.Find(Role, Id, ContractVersion);
	}

	auto FImportService::SelectTranslator(
		const FImportSourceRecognition& Source,
		std::string_view PersistedId,
		uint32 PersistedVersion) const -> FInterchangeSelectionResult
	{
		if (!PersistedId.empty())
		{
			FInterchangeComponentLease Lease = Impl->Interchange.Find(
				EInterchangeComponentRole::Translator, PersistedId, PersistedVersion);
			if (Lease) return {.Lease = std::move(Lease)};
			return {.Diagnostics = {{
				.Category = EImportDiagnosticCategory::ProviderUnavailable,
				.Identity = "InterchangePersistedTranslatorUnavailable",
				.Phase = "Selection",
				.Message = std::format("Persisted translator '{}' version {} is unavailable.",
					PersistedId, PersistedVersion)}}};
		}
		return Impl->Interchange.SelectTranslator(Source);
	}

	auto FImportService::SelectFactory(
		std::string_view OutputClassName,
		std::string_view PersistedId,
		uint32 PersistedVersion) const -> FInterchangeSelectionResult
	{
		if (!PersistedId.empty())
		{
			FInterchangeComponentLease Lease = Impl->Interchange.Find(
				EInterchangeComponentRole::Factory, PersistedId, PersistedVersion);
			if (Lease && Lease.GetOutputClassName() == OutputClassName)
				return {.Lease = std::move(Lease)};
			return {.Diagnostics = {{
				.Category = EImportDiagnosticCategory::ProviderUnavailable,
				.Identity = "InterchangePersistedFactoryUnavailable",
				.Phase = "Selection",
				.Message = std::format(
					"Persisted factory '{}' version {} is unavailable for output class '{}'.",
					PersistedId, PersistedVersion, OutputClassName)}}};
		}
		return Impl->Interchange.SelectFactory(OutputClassName);
	}

	auto FImportService::EnumerateInterchangeComponents(
		EInterchangeComponentRole Role) const
		-> std::vector<FInterchangeComponentIdentity>
	{
		return Impl->Interchange.Enumerate(Role);
	}

	auto FImportService::GetInterchangeRevision() const -> uint64
	{
		return Impl->Interchange.GetRevision();
	}

	auto FImportService::CreateImportPlan(const FImportPlanRequest& Request)
		-> FImportPlanResult
	{
		return CreateImportPlanInternal(Request, Impl->Providers);
	}

	auto GetImportService() -> FImportService&
	{
		static FImportService Service;
		return Service;
	}

}
