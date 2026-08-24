#pragma once

#include "AssetForge/Extensions/AssetBuilder.h"
#include "AssetForge/Extensions/PlanningPass.h"

namespace Durin::AssetForge
{
class FImportService;

enum class EComponentRole : uint8
	{
		Translator,
		PlanningPass,
		AssetBuilder
	};

	struct FSourceTranslatorRegistrationDescriptor
	{
		FTranslatorDescriptor Descriptor;
		std::shared_ptr<ISourceTranslator> Implementation;
	};

	struct FPlanningPassRegistrationDescriptor
	{
		FPlanningPassDescriptor Descriptor;
		std::shared_ptr<IPlanningPass> Implementation;
	};

	struct FAssetBuilderRegistrationDescriptor
	{
		FAssetBuilderDescriptor Descriptor;
		std::shared_ptr<IAssetBuilder> Implementation;
	};

	struct FComponentLeaseState;

	// Retains the implementation and its module resource until every escaped
	// factory product or invocation-owned value is destroyed.
	class FComponentLease
	{
	public:
		ASSETFORGE_API FComponentLease();
		ASSETFORGE_API ~FComponentLease();
		ASSETFORGE_API FComponentLease(const FComponentLease&);
		ASSETFORGE_API FComponentLease(FComponentLease&&) noexcept;
		ASSETFORGE_API auto operator=(const FComponentLease&)
			-> FComponentLease&;
		ASSETFORGE_API auto operator=(FComponentLease&&) noexcept
			-> FComponentLease&;

		auto IsValid() const -> bool { return State != nullptr; }
		explicit operator bool() const { return IsValid(); }
		ASSETFORGE_API auto GetRole() const -> EComponentRole;
		ASSETFORGE_API auto GetId() const -> std::string_view;
		ASSETFORGE_API auto GetContractVersion() const -> uint32;
		ASSETFORGE_API auto GetSettingsSchemaId() const -> std::string_view;
		ASSETFORGE_API auto GetSettingsSchemaVersion() const -> uint32;
		ASSETFORGE_API auto GetOutputClassName() const -> std::string_view;
		ASSETFORGE_API auto GetThreadCapability() const
			-> EThreadCapability;
		ASSETFORGE_API auto GetSourceTranslator() const -> const ISourceTranslator*;
		ASSETFORGE_API auto GetPlanningPass() const -> const IPlanningPass*;
		ASSETFORGE_API auto GetAssetBuilder() const -> const IAssetBuilder*;
		ASSETFORGE_API auto TryEnter() const -> FModuleOwnedCallbackInvocation;

		// Registry infrastructure constructs leases only from an opaque state.
		explicit FComponentLease(
			std::shared_ptr<const FComponentLeaseState> InState,
			FModuleOwnedResourceLease InResource);

	private:
		std::shared_ptr<const FComponentLeaseState> State;
		std::shared_ptr<FModuleOwnedResourceLease> ResourceLease;

	};

	// Owns one exact translator, pipeline, or factory registration identity.
	class FComponentRegistration final
	{
	public:
		FComponentRegistration() = default;
		ASSETFORGE_API ~FComponentRegistration();
		FComponentRegistration(const FComponentRegistration&) = delete;
		auto operator=(const FComponentRegistration&)
			-> FComponentRegistration& = delete;
		ASSETFORGE_API FComponentRegistration(
			FComponentRegistration&& Other) noexcept;
		ASSETFORGE_API auto operator=(FComponentRegistration&& Other) noexcept
			-> FComponentRegistration&;

		explicit operator bool() const { return Owner != nullptr; }
		ASSETFORGE_API auto Reset() -> bool;

	private:
		FComponentRegistration(
			FImportService& InOwner,
			std::weak_ptr<void> InOwnerLifetime,
			EComponentRole InRole,
			std::string InId,
			uint64 InIdentity);

		FImportService* Owner = nullptr;
		std::weak_ptr<void> OwnerLifetime;
		EComponentRole Role = EComponentRole::Translator;
		std::string Id;
		uint64 Identity = 0;

		friend class FImportService;
	};

struct FComponentSelectionResult
	{
		FComponentLease Lease;
		std::vector<FImportDiagnostic> Diagnostics;

		explicit operator bool() const { return static_cast<bool>(Lease); }
	};
}
