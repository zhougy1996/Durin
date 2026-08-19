#include "ImportService.h"
#include "ImportRegistryInternal.h"

namespace Durin::Asset
{
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
			std::vector<std::string> AssetClassNames;
			bool bHasRecordHandler = false;
			uint64 Identity = 0;
		};

		mutable std::mutex Mutex;
		FImporterStore Providers;
		FSingleAssetHandlerRegistry SingleAssetHandlers;
		FImportRecordHandlerRegistry RecordHandlers;
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
		for (const auto& Handler : Descriptor.SingleAssetHandlers)
		{
			if (!Handler || Handler->GetProviderId() != ProviderId)
			{
				OutError = "Every single-asset capability must belong to the descriptor provider.";
				return {};
			}
		}
		if (Descriptor.RecordHandler
			&& Descriptor.RecordHandler->GetProviderId() != ProviderId)
		{
			OutError = "The record capability must belong to the descriptor provider.";
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
		auto Rollback = [&]
		{
			for (auto It = Registration.AssetClassNames.rbegin();
				It != Registration.AssetClassNames.rend(); ++It)
				Impl->SingleAssetHandlers.Unregister(*It);
			if (Registration.bHasRecordHandler)
				Impl->RecordHandlers.Unregister(ProviderId);
			Impl->Providers.Unregister(ProviderId);
		};

		if (Descriptor.RecordHandler)
		{
			if (!Impl->RecordHandlers.Register(
				std::move(Descriptor.RecordHandler), OwnerGate, OutError))
			{
				Rollback();
				return {};
			}
			Registration.bHasRecordHandler = true;
		}
		for (auto& Handler : Descriptor.SingleAssetHandlers)
		{
			const std::string AssetClassName(Handler->GetAssetClassName());
			if (!Impl->SingleAssetHandlers.Register(std::move(Handler), OwnerGate, OutError))
			{
				Rollback();
				return {};
			}
			Registration.AssetClassNames.push_back(AssetClassName);
		}
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
		for (auto ClassIt = It->second.AssetClassNames.rbegin();
			ClassIt != It->second.AssetClassNames.rend(); ++ClassIt)
			Impl->SingleAssetHandlers.Unregister(*ClassIt);
		if (It->second.bHasRecordHandler)
			Impl->RecordHandlers.Unregister(ProviderId);
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
		for (auto ClassIt = It->second.AssetClassNames.rbegin();
			ClassIt != It->second.AssetClassNames.rend(); ++ClassIt)
			Impl->SingleAssetHandlers.Unregister(*ClassIt);
		if (It->second.bHasRecordHandler)
			Impl->RecordHandlers.Unregister(ProviderId);
		Impl->Providers.Unregister(ProviderId);
		Impl->Registrations.erase(It);
		++Impl->Revision;
		return true;
	}

	auto FImportService::HasSingleAssetImporter(std::string_view AssetClassName) const -> bool
	{
		return static_cast<bool>(Impl->SingleAssetHandlers.Find(AssetClassName));
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
	auto FImportService::FindSingleAssetHandler(std::string_view AssetClassName) const
		-> std::shared_ptr<const ISingleAssetImportHandler>
	{
		return Impl->SingleAssetHandlers.Find(AssetClassName);
	}
	auto FImportService::FindImportRecordHandler(std::string_view ProviderId) const
		-> std::shared_ptr<const IImportRecordHandler>
	{
		return Impl->RecordHandlers.Find(ProviderId);
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
