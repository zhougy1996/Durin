#include "Modules/ModuleManager.h"
#include "Editor/EditorNotificationSubsystem.h"

namespace Durin
{
	// Publishes editor services before shell consumers are created.
	class FDurinEdModule : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			Notifications = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{
				.Type = DEditorNotificationSubsystem::StaticClass(), .Provider = FModuleStartup::GetModuleName()});
		}
		auto ShutdownModule() -> void override { Notifications.reset(); }
	private:
		std::unique_ptr<FEditorSubsystemRegistration> Notifications;
	};
	IMPLEMENT_MODULE(FDurinEdModule, DurinEd)
}
