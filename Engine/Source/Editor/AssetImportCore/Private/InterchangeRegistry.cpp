#include "InterchangeRegistryInternal.h"

namespace Durin::Asset
{
	namespace
	{
		auto IsComponentId(std::string_view Value) -> bool
		{
			return !Value.empty() && Value.size() <= 1'024
				&& std::ranges::all_of(Value, [](char Character) {
					const unsigned char Byte = static_cast<unsigned char>(Character);
					return Byte >= 0x21 && Byte <= 0x7e && Character != '\\';
				});
		}

		auto FoldExtension(std::string Value) -> std::string
		{
			std::ranges::transform(Value, Value.begin(), [](unsigned char Character) {
				return static_cast<char>(std::tolower(Character));
			});
			return Value;
		}

		auto RoleName(EInterchangeComponentRole Role) -> std::string_view
		{
			switch (Role)
			{
			case EInterchangeComponentRole::Translator: return "translator";
			case EInterchangeComponentRole::Pipeline: return "pipeline";
			case EInterchangeComponentRole::Factory: return "factory";
			}
			return "component";
		}

		struct FRegistryKey
		{
			EInterchangeComponentRole Role = EInterchangeComponentRole::Translator;
			std::string Id;

			auto operator<=>(const FRegistryKey&) const = default;
		};
	}

	struct FInterchangeComponentLeaseState
	{
		FModuleOwnedResourceLease RegistryResource;
		std::shared_ptr<IInterchangeTranslator> Translator;
		std::shared_ptr<IInterchangePipeline> Pipeline;
		std::shared_ptr<IInterchangeFactory> Factory;
		FModuleOwnedCallbackGate OwnerGate;
		FInterchangeComponentIdentity Identity;
		EInterchangeComponentRole Role = EInterchangeComponentRole::Translator;
		std::vector<std::string> Extensions;
		std::string OutputClassName;
		EInterchangeThreadCapability ThreadCapability =
			EInterchangeThreadCapability::WorkerSafe;
		int32 Priority = 0;
		uint64 RegistrationIdentity = 0;
	};

	FInterchangeComponentLease::FInterchangeComponentLease() = default;
	FInterchangeComponentLease::~FInterchangeComponentLease() = default;
	FInterchangeComponentLease::FInterchangeComponentLease(
		const FInterchangeComponentLease&) = default;
	FInterchangeComponentLease::FInterchangeComponentLease(
		FInterchangeComponentLease&&) noexcept = default;
	auto FInterchangeComponentLease::operator=(const FInterchangeComponentLease&)
		-> FInterchangeComponentLease& = default;
	auto FInterchangeComponentLease::operator=(FInterchangeComponentLease&&) noexcept
		-> FInterchangeComponentLease& = default;

	FInterchangeComponentLease::FInterchangeComponentLease(
		std::shared_ptr<const FInterchangeComponentLeaseState> InState,
		FModuleOwnedResourceLease InResource)
		: State(std::move(InState)),
		ResourceLease(std::make_shared<FModuleOwnedResourceLease>(std::move(InResource))) {}

	auto FInterchangeComponentLease::GetRole() const -> EInterchangeComponentRole
	{
		return State ? State->Role : EInterchangeComponentRole::Translator;
	}

	auto FInterchangeComponentLease::GetId() const -> std::string_view
	{
		return State ? std::string_view(State->Identity.Id) : std::string_view{};
	}

	auto FInterchangeComponentLease::GetContractVersion() const -> uint32
	{
		return State ? State->Identity.ContractVersion : 0;
	}

	auto FInterchangeComponentLease::GetSettingsSchemaId() const -> std::string_view
	{
		return State ? std::string_view(State->Identity.Settings.SchemaId)
			: std::string_view{};
	}

	auto FInterchangeComponentLease::GetSettingsSchemaVersion() const -> uint32
	{
		return State ? State->Identity.Settings.SchemaVersion : 0;
	}

	auto FInterchangeComponentLease::GetOutputClassName() const -> std::string_view
	{
		return State ? std::string_view(State->OutputClassName) : std::string_view{};
	}

	auto FInterchangeComponentLease::GetThreadCapability() const
		-> EInterchangeThreadCapability
	{
		return State ? State->ThreadCapability
			: EInterchangeThreadCapability::EditorOnly;
	}

	auto FInterchangeComponentLease::GetTranslator() const
		-> const IInterchangeTranslator*
	{
		return State ? State->Translator.get() : nullptr;
	}

	auto FInterchangeComponentLease::GetPipeline() const -> const IInterchangePipeline*
	{
		return State ? State->Pipeline.get() : nullptr;
	}

	auto FInterchangeComponentLease::GetFactory() const -> const IInterchangeFactory*
	{
		return State ? State->Factory.get() : nullptr;
	}

	auto FInterchangeComponentLease::TryEnter() const -> FModuleOwnedCallbackInvocation
	{
		return State ? State->OwnerGate.TryEnter() : FModuleOwnedCallbackInvocation{};
	}

	struct FInterchangeRegistryStore::FImpl
	{
		mutable std::mutex Mutex;
		std::map<FRegistryKey, std::shared_ptr<FInterchangeComponentLeaseState>> Active;
		std::map<FRegistryKey, std::weak_ptr<FInterchangeComponentLeaseState>> Retired;
		uint64 Revision = 1;
		uint64 NextRegistrationIdentity = 1;
	};

	FInterchangeRegistryStore::FInterchangeRegistryStore()
		: Impl(std::make_unique<FImpl>()) {}
	FInterchangeRegistryStore::~FInterchangeRegistryStore() = default;

	namespace
	{
	auto RegisterState(
			FInterchangeRegistryStore::FImpl& Impl,
			std::shared_ptr<FInterchangeComponentLeaseState> State,
			std::string& OutError) -> FInterchangeRegistryRegistration
		{
			auto Invocation = State->OwnerGate.TryEnter();
			if (!Invocation || !IsComponentId(State->Identity.Id)
				|| State->Identity.ContractVersion == 0)
			{
				OutError = "Interchange component identity, owner, or contract version is invalid.";
				return {};
			}
			if (!State->Identity.Settings.SchemaId.empty())
			{
				std::string SettingsError;
				if (!State->Identity.Settings.Finalize(SettingsError))
				{
					OutError = std::move(SettingsError);
					return {};
				}
			}
			const FRegistryKey Key{State->Role, State->Identity.Id};
			std::lock_guard Lock(Impl.Mutex);
			if (const auto Retired = Impl.Retired.find(Key); Retired != Impl.Retired.end())
			{
				if (!Retired->second.expired())
				{
					OutError = std::format("Interchange {} '{}' still has outstanding implementation leases.",
						RoleName(State->Role), State->Identity.Id);
					return {};
				}
				Impl.Retired.erase(Retired);
			}
			if (Impl.Active.contains(Key))
			{
				OutError = std::format("Interchange {} '{}' is already registered.",
					RoleName(State->Role), State->Identity.Id);
				return {};
			}
			auto Resource = State->OwnerGate.RetainResource();
			if (!Resource)
			{
				OutError = "Interchange component owner is retiring.";
				return {};
			}
			State->RegistryResource = std::move(Resource);
			State->RegistrationIdentity = Impl.NextRegistrationIdentity++;
			const FInterchangeRegistryRegistration Registration{
				.Role = State->Role,
				.Id = State->Identity.Id,
				.Identity = State->RegistrationIdentity};
			Impl.Active.emplace(Key, std::move(State));
			++Impl.Revision;
			OutError.clear();
			return Registration;
		}
	}

	auto FInterchangeRegistryStore::Register(
		FTranslatorRegistrationDescriptor Descriptor,
		FModuleOwnedCallbackGate OwnerGate,
		std::string& OutError) -> FInterchangeRegistryRegistration
	{
		if (!Descriptor.Implementation || Descriptor.Descriptor.Extensions.empty())
		{
			OutError = "Translator registration requires an implementation and source extensions.";
			return {};
		}
		for (std::string& Extension : Descriptor.Descriptor.Extensions)
			Extension = FoldExtension(std::move(Extension));
		return RegisterState(*Impl, std::make_shared<FInterchangeComponentLeaseState>(
			FInterchangeComponentLeaseState{
				.Translator = std::move(Descriptor.Implementation),
				.OwnerGate = std::move(OwnerGate),
				.Identity = std::move(Descriptor.Descriptor.Identity),
				.Role = EInterchangeComponentRole::Translator,
				.Extensions = std::move(Descriptor.Descriptor.Extensions),
				.ThreadCapability = Descriptor.Descriptor.TranslationThread,
				.Priority = Descriptor.Descriptor.Priority}), OutError);
	}

	auto FInterchangeRegistryStore::Register(
		FPipelineRegistrationDescriptor Descriptor,
		FModuleOwnedCallbackGate OwnerGate,
		std::string& OutError) -> FInterchangeRegistryRegistration
	{
		if (!Descriptor.Implementation)
		{
			OutError = "Pipeline registration requires an implementation.";
			return {};
		}
		return RegisterState(*Impl, std::make_shared<FInterchangeComponentLeaseState>(
			FInterchangeComponentLeaseState{
				.Pipeline = std::move(Descriptor.Implementation),
				.OwnerGate = std::move(OwnerGate),
				.Identity = std::move(Descriptor.Descriptor.Identity),
				.Role = EInterchangeComponentRole::Pipeline,
				.ThreadCapability = Descriptor.Descriptor.ExecutionThread,
				.Priority = Descriptor.Descriptor.Priority}), OutError);
	}

	auto FInterchangeRegistryStore::Register(
		FFactoryRegistrationDescriptor Descriptor,
		FModuleOwnedCallbackGate OwnerGate,
		std::string& OutError) -> FInterchangeRegistryRegistration
	{
		if (!Descriptor.Implementation || Descriptor.Descriptor.OutputClassName.empty())
		{
			OutError = "Factory registration requires an implementation and output class.";
			return {};
		}
		return RegisterState(*Impl, std::make_shared<FInterchangeComponentLeaseState>(
			FInterchangeComponentLeaseState{
				.Factory = std::move(Descriptor.Implementation),
				.OwnerGate = std::move(OwnerGate),
				.Identity = std::move(Descriptor.Descriptor.Identity),
				.Role = EInterchangeComponentRole::Factory,
				.OutputClassName = std::move(Descriptor.Descriptor.OutputClassName),
				.ThreadCapability = Descriptor.Descriptor.ProductBuildThread,
				.Priority = Descriptor.Descriptor.Priority}), OutError);
	}

	auto FInterchangeRegistryStore::Unregister(
		EInterchangeComponentRole Role, std::string_view Id, uint64 Identity) -> bool
	{
		std::lock_guard Lock(Impl->Mutex);
		const FRegistryKey Key{Role, std::string(Id)};
		const auto It = Impl->Active.find(Key);
		if (It == Impl->Active.end() || It->second->RegistrationIdentity != Identity)
			return false;
		Impl->Retired[It->first] = It->second;
		Impl->Active.erase(It);
		++Impl->Revision;
		return true;
	}

	auto FInterchangeRegistryStore::Find(
		EInterchangeComponentRole Role,
		std::string_view Id,
		uint32 ContractVersion) const -> FInterchangeComponentLease
	{
		std::lock_guard Lock(Impl->Mutex);
		const auto It = Impl->Active.find({Role, std::string(Id)});
		if (It == Impl->Active.end()
			|| (ContractVersion != 0 && It->second->Identity.ContractVersion != ContractVersion))
			return {};
		auto Invocation = It->second->OwnerGate.TryEnter();
		auto Resource = It->second->OwnerGate.RetainResource();
		if (!Invocation || !Resource) return {};
		return FInterchangeComponentLease(It->second, std::move(Resource));
	}

	namespace
	{
		template<typename TPredicate>
		auto SelectMatching(
			const FInterchangeRegistryStore::FImpl& Impl,
			EInterchangeComponentRole Role,
			TPredicate&& Predicate) -> FInterchangeSelectionResult
		{
			std::lock_guard Lock(Impl.Mutex);
			std::vector<std::shared_ptr<FInterchangeComponentLeaseState>> Matches;
			for (const auto& [Key, State] : Impl.Active)
				if (Key.Role == Role && Predicate(*State)) Matches.push_back(State);
			if (Matches.empty())
			{
				return {.Diagnostics = {{
					.Category = EImportDiagnosticCategory::ProviderUnavailable,
					.Identity = "InterchangeComponentUnavailable",
					.Phase = "Selection",
					.Message = std::format("No compatible interchange {} is registered.", RoleName(Role))}}};
			}
			const int32 BestPriority = std::ranges::max(Matches, {},
				&FInterchangeComponentLeaseState::Priority)->Priority;
			std::erase_if(Matches, [BestPriority](const auto& State) {
				return State->Priority != BestPriority;
			});
			if (Matches.size() != 1)
			{
				return {.Diagnostics = {{
					.Category = EImportDiagnosticCategory::ProviderAmbiguous,
					.Identity = "InterchangeComponentAmbiguous",
					.Phase = "Selection",
					.Message = std::format("{} interchange {} registrations share priority {}.",
						Matches.size(), RoleName(Role), BestPriority)}}};
			}
			auto Invocation = Matches.front()->OwnerGate.TryEnter();
			auto Resource = Matches.front()->OwnerGate.RetainResource();
			if (!Invocation || !Resource) return {};
			return {.Lease = FInterchangeComponentLease(Matches.front(), std::move(Resource))};
		}
	}

	auto FInterchangeRegistryStore::SelectTranslator(
		const FImportSourceRecognition& Source) const -> FInterchangeSelectionResult
	{
		const std::string Extension = FoldExtension(Source.Extension);
		return SelectMatching(*Impl, EInterchangeComponentRole::Translator,
			[&](const FInterchangeComponentLeaseState& State) {
				if (std::ranges::find(State.Extensions, Extension) == State.Extensions.end())
					return false;
				auto Invocation = State.OwnerGate.TryEnter();
				return Invocation && State.Translator->Recognize(Source);
			});
	}

	auto FInterchangeRegistryStore::SelectFactory(
		std::string_view OutputClassName) const -> FInterchangeSelectionResult
	{
		return SelectMatching(*Impl, EInterchangeComponentRole::Factory,
			[&](const FInterchangeComponentLeaseState& State) {
				return State.OutputClassName == OutputClassName;
			});
	}

	auto FInterchangeRegistryStore::Enumerate(EInterchangeComponentRole Role) const
		-> std::vector<FInterchangeComponentIdentity>
	{
		std::lock_guard Lock(Impl->Mutex);
		std::vector<FInterchangeComponentIdentity> Result;
		for (const auto& [Key, State] : Impl->Active)
			if (Key.Role == Role) Result.push_back({
				.Id = State->Identity.Id,
				.ContractVersion = State->Identity.ContractVersion});
		return Result;
	}

	auto FInterchangeRegistryStore::GetRevision() const -> uint64
	{
		std::lock_guard Lock(Impl->Mutex);
		return Impl->Revision;
	}

	auto FInterchangeRegistryStore::GetOutstandingLeaseCount(
		EInterchangeComponentRole Role, std::string_view Id) const -> uint64
	{
		std::lock_guard Lock(Impl->Mutex);
		const FRegistryKey Key{Role, std::string(Id)};
		if (const auto Active = Impl->Active.find(Key); Active != Impl->Active.end())
			return Active->second.use_count() > 0 ? Active->second.use_count() - 1 : 0;
		const auto Retired = Impl->Retired.find(Key);
		if (Retired == Impl->Retired.end()) return 0;
		const std::shared_ptr<FInterchangeComponentLeaseState> State = Retired->second.lock();
		return State && State.use_count() > 0 ? State.use_count() - 1 : 0;
	}
}
