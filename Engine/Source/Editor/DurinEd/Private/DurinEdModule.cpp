#include "Modules/ModuleManager.h"
#include "Actors/EditorActorSubsystem.h"
#include "Notifications/EditorNotificationSubsystem.h"
#include "Tools/EditorToolSubsystem.h"
#include "Tools/PIEToolset.h"

namespace Durin
{
	// Publishes editor services before shell consumers are created.
	class FDurinEdModule : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			Actors = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{
				.Type = DEditorActorSubsystem::StaticClass(), .Provider = FModuleStartup::GetModuleName()});
			Notifications = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{
				.Type = DEditorNotificationSubsystem::StaticClass(), .Provider = FModuleStartup::GetModuleName()});
			Tools = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{
				.Type = DEditorToolSubsystem::StaticClass(), .Provider = FModuleStartup::GetModuleName()});
			PIE = CreatePIEToolset();
			PIERegistration = FModularFeatureRegistry::Get().RegisterFeature(*PIE);
		}
		auto ShutdownModule() -> void override { PIERegistration.Reset(); PIE.reset(); Tools.reset(); Notifications.reset(); Actors.reset(); }
	private:
		std::unique_ptr<FEditorSubsystemRegistration> Actors;
		std::unique_ptr<FNativeToolset> PIE;
		FModularFeatureRegistration PIERegistration;
		std::unique_ptr<FEditorSubsystemRegistration> Notifications;
		std::unique_ptr<FEditorSubsystemRegistration> Tools;
	};
	IMPLEMENT_MODULE(FDurinEdModule, DurinEd)
}
