#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "ContentBrowserFileProviderSubsystem.h"
#include "Editor/EditorEngine.h"
namespace Durin
{
	class FContentBrowserFileDataSourceModule final : public IModuleInterface
	{
		std::unique_ptr<FEditorSubsystemRegistration> Registration;
	public:
		auto StartupModule() -> void override {
			Registration = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{.Type = DContentBrowserFileProviderSubsystem::StaticClass(), .Provider = FName("ContentBrowserFileDataSource")}); }
		auto ShutdownModule() -> void override { if (GEditor) GEditor->RemoveEditorSubsystems(FName("ContentBrowserFileDataSource")); Registration.reset(); }
	};
	IMPLEMENT_MODULE(FContentBrowserFileDataSourceModule, ContentBrowserFileDataSource)
}
