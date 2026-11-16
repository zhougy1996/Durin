#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "ContentBrowserFileProviderSubsystem.h"
#include "Editor/EditorEngine.h"
#include "AssetTools/DeleteItemsHost.h"
#include "Data/FileDeletionRequest.h"
namespace Durin
{
	class FContentBrowserFileDataSourceModule final : public IModuleInterface
	{
		std::unique_ptr<FEditorSubsystemRegistration> Registration;
	public:
		auto StartupModule() -> void override {
			ObjectTools::Private::SetFolderDeleteHandler(ContentStorage::FFileDeletionRequest::DeleteObjectsAndFolders);
			Registration = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{.Type = DContentBrowserFileProviderSubsystem::StaticClass(), .Provider = FName("ContentBrowserFileDataSource")}); }
		auto ShutdownModule() -> void override { ObjectTools::Private::SetFolderDeleteHandler({}); if (GEditor) GEditor->RemoveEditorSubsystems(FName("ContentBrowserFileDataSource")); Registration.reset(); }
	};
	IMPLEMENT_MODULE(FContentBrowserFileDataSourceModule, ContentBrowserFileDataSource)
}
