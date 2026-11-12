#include "CoreMinimal.h"
#include "ContentBrowserDataSubsystem.h"
#include "Editor/EditorEngine.h"
namespace Durin
{
	class FContentBrowserModule final : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			Registration = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{.Type = DContentBrowserDataSubsystem::StaticClass(), .Provider = FName("ContentBrowser")});
		}
		auto ShutdownModule() -> void override
		{
			if (GEditor) GEditor->RemoveEditorSubsystems(FName("ContentBrowser"));
			Registration.reset();
		}

	private:
		std::unique_ptr<FEditorSubsystemRegistration> Registration;
	};
	IMPLEMENT_MODULE(FContentBrowserModule, ContentBrowser)
} // namespace Durin
