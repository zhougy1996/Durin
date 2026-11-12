#include "CoreMinimal.h"
#include "ContentBrowserDataSubsystem.h"
#include "ContentBrowserProviderSubsystems.h"
#include "Editor/EditorEngine.h"
namespace Durin
{
	class FContentBrowserModule final : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			for (auto* Type : {DContentBrowserDataSubsystem::StaticClass(), DContentBrowserAssetProviderSubsystem::StaticClass(), DContentBrowserFileProviderSubsystem::StaticClass()})
				Registrations.push_back(std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{.Type = Type, .Provider = FName("ContentBrowser")}));
		}
		auto ShutdownModule() -> void override
		{
			if (GEditor) GEditor->RemoveEditorSubsystems(FName("ContentBrowser"));
			Registrations.clear();
		}

	private:
		std::vector<std::unique_ptr<FEditorSubsystemRegistration>> Registrations;
	};
	IMPLEMENT_MODULE(FContentBrowserModule, ContentBrowser)
} // namespace Durin
