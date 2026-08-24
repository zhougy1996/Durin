#include "AssetForgeBuiltinsProviders.h"

#include "BuiltinProviderRegistration.h"
#include "ImageFamilyImports.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
		std::mutex GRegistrationMutex;
		std::vector<FComponentRegistration> GRegistrations;
		uint32 GRegistrationReferenceCount = 0;
	}

	auto RegisterAssetForgeBuiltinsProviders(
		std::string& OutError,
		FModuleOwnedCallbackGate OwnerGate) -> bool
	{
		std::lock_guard Lock(GRegistrationMutex);
		if (GRegistrationReferenceCount != 0)
		{
			++GRegistrationReferenceCount;
			OutError.clear();
			return true;
		}

		auto& Service = GetImportService();
		GRegistrations.reserve(21);
		if (!RegisterTexture2DImportProvider(
				Service, OwnerGate, GRegistrations, OutError)
			|| !RegisterStaticMeshImportProvider(
				Service, OwnerGate, GRegistrations, OutError)
			|| !RegisterSceneImportProvider(
				Service, OwnerGate, GRegistrations, OutError)
			|| !RegisterImageFamilyImports(
				Service, std::move(OwnerGate), GRegistrations, OutError))
		{
			GRegistrations.clear();
			ClearSceneImportProviderCaches();
			return false;
		}

		GRegistrationReferenceCount = 1;
		OutError.clear();
		return true;
	}

	auto UnregisterAssetForgeBuiltinsProviders() -> void
	{
		std::lock_guard Lock(GRegistrationMutex);
		if (GRegistrationReferenceCount > 1)
		{
			--GRegistrationReferenceCount;
			return;
		}
		GRegistrationReferenceCount = 0;
		GRegistrations.clear();
		ClearSceneImportProviderCaches();
	}
}
