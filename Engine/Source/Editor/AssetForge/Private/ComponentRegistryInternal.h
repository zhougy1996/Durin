#pragma once

#include "AssetForge/Extensions/ComponentRegistration.h"

namespace Durin::AssetForge::Private
{
	struct FComponentRegistryRegistration
	{
		EComponentRole Role = EComponentRole::Translator;
		std::string Id;
		uint64 Identity = 0;
	};

	class FComponentRegistryStore final
	{
	public:
		struct FImpl;

		FComponentRegistryStore();
		~FComponentRegistryStore();
		FComponentRegistryStore(const FComponentRegistryStore&) = delete;
		auto operator=(const FComponentRegistryStore&)
			-> FComponentRegistryStore& = delete;

		auto Register(FSourceTranslatorRegistrationDescriptor Descriptor,
			FModuleOwnedCallbackGate OwnerGate, std::string& OutError)
			-> FComponentRegistryRegistration;
		auto Register(FPlanningPassRegistrationDescriptor Descriptor,
			FModuleOwnedCallbackGate OwnerGate, std::string& OutError)
			-> FComponentRegistryRegistration;
		auto Register(FAssetBuilderRegistrationDescriptor Descriptor,
			FModuleOwnedCallbackGate OwnerGate, std::string& OutError)
			-> FComponentRegistryRegistration;
		auto Unregister(EComponentRole Role,
			std::string_view Id, uint64 Identity) -> bool;
		auto Find(EComponentRole Role,
			std::string_view Id, uint32 ContractVersion = 0) const
			-> FComponentLease;
		auto SelectSourceTranslator(const FImportSourceRecognition& Source) const
			-> FComponentSelectionResult;
		auto SelectAssetBuilder(std::string_view OutputClassName) const
			-> FComponentSelectionResult;
		auto Enumerate(EComponentRole Role) const
			-> std::vector<FComponentIdentity>;
		auto GetRevision() const -> uint64;
		auto GetOutstandingLeaseCount(EComponentRole Role,
			std::string_view Id) const -> uint64;

	private:
		std::unique_ptr<FImpl> Impl;
	};
}
