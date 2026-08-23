#pragma once

#include "Interchange.h"

namespace Durin::Asset
{
	class FImportService;
}

namespace Durin::Asset::Forge
{
	auto RegisterImageFamilyInterchange(
		FImportService& Service,
		FModuleOwnedCallbackGate OwnerGate,
		std::vector<FInterchangeRegistration>& OutRegistrations,
		std::string& OutError) -> bool;
}
