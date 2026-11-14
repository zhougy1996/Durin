#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "ContentBrowserAssetProviderSubsystem.h"
#include "Editor/EditorEngine.h"
namespace Durin
{
	class FContentBrowserAssetDataSourceModule final : public IModuleInterface
	{
		std::unique_ptr<FEditorSubsystemRegistration> Registration;
	public:
		auto StartupModule() -> void override { Registration = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{.Type = DContentBrowserAssetProviderSubsystem::StaticClass(), .Provider = FName("ContentBrowserAssetDataSource")}); }
		auto ShutdownModule() -> void override { if (GEditor) GEditor->RemoveEditorSubsystems(FName("ContentBrowserAssetDataSource")); Registration.reset(); }
	};
	IMPLEMENT_MODULE(FContentBrowserAssetDataSourceModule, ContentBrowserAssetDataSource)
}
