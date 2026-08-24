#include "ComponentRegistryInternal.h"

namespace Durin::AssetForge
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

		auto RoleName(EComponentRole Role) -> std::string_view
		{
			switch (Role)
			{
			case EComponentRole::Translator: return "translator";
			case EComponentRole::PlanningPass: return "pipeline";
			case EComponentRole::AssetBuilder: return "factory";
			}
			return "component";
		}

		struct FRegistryKey
		{
			EComponentRole Role = EComponentRole::Translator;
			std::string Id;

			auto operator<=>(const FRegistryKey&) const = default;
		};
	}

	struct FComponentLeaseState
	{
		FModuleOwnedResourceLease RegistryResource;
		std::shared_ptr<ISourceTranslator> Translator;
		std::shared_ptr<IPlanningPass> PlanningPass;
		std::shared_ptr<IAssetBuilder> AssetBuilder;
		FModuleOwnedCallbackGate OwnerGate;
		FComponentIdentity Identity;
		EComponentRole Role = EComponentRole::Translator;
		std::vector<std::string> Extensions;
		std::string OutputClassName;
		EThreadCapability ThreadCapability =
			EThreadCapability::WorkerSafe;
		int32 Priority = 0;
		uint64 RegistrationIdentity = 0;
	};

	FComponentLease::FComponentLease() = default;
	FComponentLease::~FComponentLease() = default;
	FComponentLease::FComponentLease(
		const FComponentLease&) = default;
	FComponentLease::FComponentLease(
		FComponentLease&&) noexcept = default;
	auto FComponentLease::operator=(const FComponentLease&)
		-> FComponentLease& = default;
	auto FComponentLease::operator=(FComponentLease&&) noexcept
		-> FComponentLease& = default;

	FComponentLease::FComponentLease(
		std::shared_ptr<const FComponentLeaseState> InState,
		FModuleOwnedResourceLease InResource)
		: State(std::move(InState)),
		ResourceLease(std::make_shared<FModuleOwnedResourceLease>(std::move(InResource))) {}

	auto FComponentLease::GetRole() const -> EComponentRole
	{
		return State ? State->Role : EComponentRole::Translator;
	}

	auto FComponentLease::GetId() const -> std::string_view
	{
		return State ? std::string_view(State->Identity.Id) : std::string_view{};
	}

	auto FComponentLease::GetContractVersion() const -> uint32
	{
		return State ? State->Identity.ContractVersion : 0;
	}

	auto FComponentLease::GetSettingsSchemaId() const -> std::string_view
	{
		return State ? std::string_view(State->Identity.Settings.SchemaId)
			: std::string_view{};
	}

	auto FComponentLease::GetSettingsSchemaVersion() const -> uint32
	{
		return State ? State->Identity.Settings.SchemaVersion : 0;
	}

	auto FComponentLease::GetOutputClassName() const -> std::string_view
	{
		return State ? std::string_view(State->OutputClassName) : std::string_view{};
	}

	auto FComponentLease::GetThreadCapability() const
		-> EThreadCapability
	{
		return State ? State->ThreadCapability
			: EThreadCapability::EditorOnly;
	}

	auto FComponentLease::GetSourceTranslator() const
		-> const ISourceTranslator*
	{
		return State ? State->Translator.get() : nullptr;
	}

	auto FComponentLease::GetPlanningPass() const -> const IPlanningPass*
	{
		return State ? State->PlanningPass.get() : nullptr;
	}

	auto FComponentLease::GetAssetBuilder() const -> const IAssetBuilder*
	{
		return State ? State->AssetBuilder.get() : nullptr;
	}

	auto FComponentLease::TryEnter() const -> FModuleOwnedCallbackInvocation
	{
		return State ? State->OwnerGate.TryEnter() : FModuleOwnedCallbackInvocation{};
	}

	namespace Private
	{
	struct FComponentRegistryStore::FImpl
	{
		mutable std::mutex Mutex;
		std::map<FRegistryKey, std::shared_ptr<FComponentLeaseState>> Active;
		std::map<FRegistryKey, std::weak_ptr<FComponentLeaseState>> Retired;
		uint64 Revision = 1;
		uint64 NextRegistrationIdentity = 1;
	};

	FComponentRegistryStore::FComponentRegistryStore()
		: Impl(std::make_unique<FImpl>()) {}
	FComponentRegistryStore::~FComponentRegistryStore() = default;

	namespace
	{
	auto RegisterState(
			FComponentRegistryStore::FImpl& Impl,
			std::shared_ptr<FComponentLeaseState> State,
			std::string& OutError) -> FComponentRegistryRegistration
		{
			auto Invocation = State->OwnerGate.TryEnter();
			if (!Invocation || !IsComponentId(State->Identity.Id)
				|| State->Identity.ContractVersion == 0)
			{
				OutError = "AssetForge component identity, owner, or contract version is invalid.";
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
					OutError = std::format("AssetForge {} '{}' still has outstanding implementation leases.",
						RoleName(State->Role), State->Identity.Id);
					return {};
				}
				Impl.Retired.erase(Retired);
			}
			if (Impl.Active.contains(Key))
			{
				OutError = std::format("AssetForge {} '{}' is already registered.",
					RoleName(State->Role), State->Identity.Id);
				return {};
			}
			auto Resource = State->OwnerGate.RetainResource();
			if (!Resource)
			{
				OutError = "AssetForge component owner is retiring.";
				return {};
			}
			State->RegistryResource = std::move(Resource);
			State->RegistrationIdentity = Impl.NextRegistrationIdentity++;
			const FComponentRegistryRegistration Registration{
				.Role = State->Role,
				.Id = State->Identity.Id,
				.Identity = State->RegistrationIdentity};
			Impl.Active.emplace(Key, std::move(State));
			++Impl.Revision;
			OutError.clear();
			return Registration;
		}
	}

	auto FComponentRegistryStore::Register(
		FSourceTranslatorRegistrationDescriptor Descriptor,
		FModuleOwnedCallbackGate OwnerGate,
		std::string& OutError) -> FComponentRegistryRegistration
	{
		if (!Descriptor.Implementation || Descriptor.Descriptor.Extensions.empty())
		{
			OutError = "Translator registration requires an implementation and source extensions.";
			return {};
		}
		for (std::string& Extension : Descriptor.Descriptor.Extensions)
			Extension = FoldExtension(std::move(Extension));
		return RegisterState(*Impl, std::make_shared<FComponentLeaseState>(
			FComponentLeaseState{
				.Translator = std::move(Descriptor.Implementation),
				.OwnerGate = std::move(OwnerGate),
				.Identity = std::move(Descriptor.Descriptor.Identity),
				.Role = EComponentRole::Translator,
				.Extensions = std::move(Descriptor.Descriptor.Extensions),
				.ThreadCapability = Descriptor.Descriptor.TranslationThread,
				.Priority = Descriptor.Descriptor.Priority}), OutError);
	}

	auto FComponentRegistryStore::Register(
		FPlanningPassRegistrationDescriptor Descriptor,
		FModuleOwnedCallbackGate OwnerGate,
		std::string& OutError) -> FComponentRegistryRegistration
	{
		if (!Descriptor.Implementation)
		{
			OutError = "PlanningPass registration requires an implementation.";
			return {};
		}
		return RegisterState(*Impl, std::make_shared<FComponentLeaseState>(
			FComponentLeaseState{
				.PlanningPass = std::move(Descriptor.Implementation),
				.OwnerGate = std::move(OwnerGate),
				.Identity = std::move(Descriptor.Descriptor.Identity),
				.Role = EComponentRole::PlanningPass,
				.ThreadCapability = Descriptor.Descriptor.ExecutionThread,
				.Priority = Descriptor.Descriptor.Priority}), OutError);
	}

	auto FComponentRegistryStore::Register(
		FAssetBuilderRegistrationDescriptor Descriptor,
		FModuleOwnedCallbackGate OwnerGate,
		std::string& OutError) -> FComponentRegistryRegistration
	{
		if (!Descriptor.Implementation || Descriptor.Descriptor.OutputClassName.empty())
		{
			OutError = "AssetBuilder registration requires an implementation and output class.";
			return {};
		}
		return RegisterState(*Impl, std::make_shared<FComponentLeaseState>(
			FComponentLeaseState{
				.AssetBuilder = std::move(Descriptor.Implementation),
				.OwnerGate = std::move(OwnerGate),
				.Identity = std::move(Descriptor.Descriptor.Identity),
				.Role = EComponentRole::AssetBuilder,
				.OutputClassName = std::move(Descriptor.Descriptor.OutputClassName),
				.ThreadCapability = Descriptor.Descriptor.ProductBuildThread,
				.Priority = Descriptor.Descriptor.Priority}), OutError);
	}

	auto FComponentRegistryStore::Unregister(
		EComponentRole Role, std::string_view Id, uint64 Identity) -> bool
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

	auto FComponentRegistryStore::Find(
		EComponentRole Role,
		std::string_view Id,
		uint32 ContractVersion) const -> FComponentLease
	{
		std::lock_guard Lock(Impl->Mutex);
		const auto It = Impl->Active.find({Role, std::string(Id)});
		if (It == Impl->Active.end()
			|| (ContractVersion != 0 && It->second->Identity.ContractVersion != ContractVersion))
			return {};
		auto Invocation = It->second->OwnerGate.TryEnter();
		auto Resource = It->second->OwnerGate.RetainResource();
		if (!Invocation || !Resource) return {};
		return FComponentLease(It->second, std::move(Resource));
	}

	namespace
	{
		template<typename TPredicate>
		auto SelectMatching(
			const FComponentRegistryStore::FImpl& Impl,
			EComponentRole Role,
			TPredicate&& Predicate) -> FComponentSelectionResult
		{
			std::lock_guard Lock(Impl.Mutex);
			std::vector<std::shared_ptr<FComponentLeaseState>> Matches;
			for (const auto& [Key, State] : Impl.Active)
				if (Key.Role == Role && Predicate(*State)) Matches.push_back(State);
			if (Matches.empty())
			{
				return {.Diagnostics = {{
					.Category = EImportDiagnosticCategory::ProviderUnavailable,
					.Identity = "Durin.AssetForge.Diagnostic.ComponentUnavailable",
					.Phase = "Selection",
					.Message = std::format("No compatible interchange {} is registered.", RoleName(Role))}}};
			}
			const int32 BestPriority = std::ranges::max(Matches, {},
				&FComponentLeaseState::Priority)->Priority;
			std::erase_if(Matches, [BestPriority](const auto& State) {
				return State->Priority != BestPriority;
			});
			if (Matches.size() != 1)
			{
				return {.Diagnostics = {{
					.Category = EImportDiagnosticCategory::ProviderAmbiguous,
					.Identity = "Durin.AssetForge.Diagnostic.ComponentAmbiguous",
					.Phase = "Selection",
					.Message = std::format("{} interchange {} registrations share priority {}.",
						Matches.size(), RoleName(Role), BestPriority)}}};
			}
			auto Invocation = Matches.front()->OwnerGate.TryEnter();
			auto Resource = Matches.front()->OwnerGate.RetainResource();
			if (!Invocation || !Resource) return {};
			return {.Lease = FComponentLease(Matches.front(), std::move(Resource))};
		}
	}

	auto FComponentRegistryStore::SelectSourceTranslator(
		const FImportSourceRecognition& Source) const -> FComponentSelectionResult
	{
		const std::string Extension = FoldExtension(Source.Extension);
		return SelectMatching(*Impl, EComponentRole::Translator,
			[&](const FComponentLeaseState& State) {
				if (std::ranges::find(State.Extensions, Extension) == State.Extensions.end())
					return false;
				auto Invocation = State.OwnerGate.TryEnter();
				return Invocation && State.Translator->Recognize(Source);
			});
	}

	auto FComponentRegistryStore::SelectAssetBuilder(
		std::string_view OutputClassName) const -> FComponentSelectionResult
	{
		return SelectMatching(*Impl, EComponentRole::AssetBuilder,
			[&](const FComponentLeaseState& State) {
				return State.OutputClassName == OutputClassName;
			});
	}

	auto FComponentRegistryStore::Enumerate(EComponentRole Role) const
		-> std::vector<FComponentIdentity>
	{
		std::lock_guard Lock(Impl->Mutex);
		std::vector<FComponentIdentity> Result;
		for (const auto& [Key, State] : Impl->Active)
			if (Key.Role == Role) Result.push_back({
				.Id = State->Identity.Id,
				.ContractVersion = State->Identity.ContractVersion});
		return Result;
	}

	auto FComponentRegistryStore::GetRevision() const -> uint64
	{
		std::lock_guard Lock(Impl->Mutex);
		return Impl->Revision;
	}

	auto FComponentRegistryStore::GetOutstandingLeaseCount(
		EComponentRole Role, std::string_view Id) const -> uint64
	{
		std::lock_guard Lock(Impl->Mutex);
		const FRegistryKey Key{Role, std::string(Id)};
		if (const auto Active = Impl->Active.find(Key); Active != Impl->Active.end())
			return Active->second.use_count() > 0 ? Active->second.use_count() - 1 : 0;
		const auto Retired = Impl->Retired.find(Key);
		if (Retired == Impl->Retired.end()) return 0;
		const std::shared_ptr<FComponentLeaseState> State = Retired->second.lock();
		return State && State.use_count() > 0 ? State.use_count() - 1 : 0;
	}
	}
}
