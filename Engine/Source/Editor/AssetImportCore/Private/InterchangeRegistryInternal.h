#pragma once

#include "Interchange.h"

namespace Durin::Asset
{
	struct FInterchangeRegistryRegistration
	{
		EInterchangeComponentRole Role = EInterchangeComponentRole::Translator;
		std::string Id;
		uint64 Identity = 0;
	};

	class FInterchangeRegistryStore final
	{
	public:
		struct FImpl;

		FInterchangeRegistryStore();
		~FInterchangeRegistryStore();
		FInterchangeRegistryStore(const FInterchangeRegistryStore&) = delete;
		auto operator=(const FInterchangeRegistryStore&)
			-> FInterchangeRegistryStore& = delete;

		auto Register(FTranslatorRegistrationDescriptor Descriptor,
			FModuleOwnedCallbackGate OwnerGate, std::string& OutError)
			-> FInterchangeRegistryRegistration;
		auto Register(FPipelineRegistrationDescriptor Descriptor,
			FModuleOwnedCallbackGate OwnerGate, std::string& OutError)
			-> FInterchangeRegistryRegistration;
		auto Register(FFactoryRegistrationDescriptor Descriptor,
			FModuleOwnedCallbackGate OwnerGate, std::string& OutError)
			-> FInterchangeRegistryRegistration;
		auto Unregister(EInterchangeComponentRole Role,
			std::string_view Id, uint64 Identity) -> bool;
		auto Find(EInterchangeComponentRole Role,
			std::string_view Id, uint32 ContractVersion = 0) const
			-> FInterchangeComponentLease;
		auto SelectTranslator(const FImportSourceRecognition& Source) const
			-> FInterchangeSelectionResult;
		auto SelectFactory(std::string_view OutputClassName) const
			-> FInterchangeSelectionResult;
		auto Enumerate(EInterchangeComponentRole Role) const
			-> std::vector<FInterchangeComponentIdentity>;
		auto GetRevision() const -> uint64;
		auto GetOutstandingLeaseCount(EInterchangeComponentRole Role,
			std::string_view Id) const -> uint64;

	private:
		std::unique_ptr<FImpl> Impl;
	};
}
