#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditor/DetailCustomizationRegistry.h"

#include "MaterialEditorAPI.h"

namespace Durin::Editor
{
	class DThumbnailManager;
	class FThumbnailRendererRegistrationHandle;
}

namespace Durin
{
	// Publishes independent material definitions, creation, details and thumbnails.
	class FMaterialEditorModule final : public IModuleInterface
	{
	public:
		MATERIALEDITOR_API FMaterialEditorModule();
		MATERIALEDITOR_API ~FMaterialEditorModule() override;
		MATERIALEDITOR_API auto StartupModule() -> void override;
		MATERIALEDITOR_API auto ShutdownModule() -> void override;

	private:
		auto RegisterContributions(Editor::DThumbnailManager& ThumbnailManager) -> bool;
		auto UnregisterContributions() -> void;
		Editor::FDetailCustomizationHandle DetailCustomizationRegistration;
		std::unique_ptr<Editor::FThumbnailRendererRegistrationHandle> MaterialThumbnailRegistration;
		std::unique_ptr<Editor::FThumbnailRendererRegistrationHandle> MaterialInstanceThumbnailRegistration;
		struct FIntegrationState;
		std::unique_ptr<FIntegrationState> Integration;
	};
}
