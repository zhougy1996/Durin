#pragma once

#include "AssetForge/ImportService.h"

namespace Durin::AssetForge::Builtins
{
	auto RegisterTexture2DImportProvider(
		FImportService& Service,
		FModuleOwnedCallbackGate OwnerGate,
		std::vector<FComponentRegistration>& Registrations,
		std::string& OutError) -> bool;
	auto RegisterStaticMeshImportProvider(
		FImportService& Service,
		FModuleOwnedCallbackGate OwnerGate,
		std::vector<FComponentRegistration>& Registrations,
		std::string& OutError) -> bool;
	auto RegisterSceneImportProvider(
		FImportService& Service,
		FModuleOwnedCallbackGate OwnerGate,
		std::vector<FComponentRegistration>& Registrations,
		std::string& OutError) -> bool;
	auto ClearSceneImportProviderCaches() -> void;
}
