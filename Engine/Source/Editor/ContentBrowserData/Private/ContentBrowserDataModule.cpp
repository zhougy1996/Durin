#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "ContentBrowserDataSubsystem.h"
#include "Editor/EditorEngine.h"
namespace Durin
{
	class FContentBrowserDataModule final : public IModuleInterface
	{
		std::unique_ptr<FEditorSubsystemRegistration> Registration;
	public:
		auto StartupModule() -> void override { Registration = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{.Type = DContentBrowserDataSubsystem::StaticClass(), .Provider = FName("ContentBrowserData")}); }
		auto ShutdownModule() -> void override { if (GEditor) GEditor->RemoveEditorSubsystems(FName("ContentBrowserData")); Registration.reset(); }
	};
	IMPLEMENT_MODULE(FContentBrowserDataModule, ContentBrowserData)
}
