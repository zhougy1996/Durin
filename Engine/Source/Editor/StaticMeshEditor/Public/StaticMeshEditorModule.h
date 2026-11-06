#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

#include "StaticMeshEditorAPI.h"

namespace Durin::Editor
{
	class DThumbnailManager;
	class FThumbnailRendererRegistrationHandle;
}

namespace Durin
{
	// Publishes mesh definitions, import and thumbnails; sessions own inspectors.
	class FStaticMeshEditorModule final : public IModuleInterface
	{
	public:
		STATICMESHEDITOR_API FStaticMeshEditorModule();
		STATICMESHEDITOR_API ~FStaticMeshEditorModule() override;
		STATICMESHEDITOR_API auto StartupModule() -> void override;
		STATICMESHEDITOR_API auto ShutdownModule() -> void override;
	private:
		auto RegisterContributions(Editor::DThumbnailManager& ThumbnailManager) -> bool;
		auto UnregisterContributions() -> void;
		std::unique_ptr<Editor::FThumbnailRendererRegistrationHandle> ThumbnailRegistration;
		struct FIntegrationState;
		std::unique_ptr<FIntegrationState> Integration;
	};
}
