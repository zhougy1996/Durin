#include "Modules/ModuleManager.h"
#include "Preview/TexturePreview.h"
#include "Editor/EditorEngine.h"
#include "DObject/ObjectLifecycle.h"
#include "Actors/EditorActorSubsystem.h"
#include "Notifications/EditorNotificationSubsystem.h"
#include "Tools/EditorToolSubsystem.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace Durin
{
	// Publishes editor services before shell consumers are created.
	class FDurinEdModule : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			Provider = FModuleStartup::GetModuleName();
			Actors = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{
				.Type = DEditorActorSubsystem::StaticClass(), .Provider = FModuleStartup::GetModuleName()});
			Notifications = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{
				.Type = DEditorNotificationSubsystem::StaticClass(), .Provider = FModuleStartup::GetModuleName()});
			Tools = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{
				.Type = DEditorToolSubsystem::StaticClass(), .Provider = FModuleStartup::GetModuleName()});
			AssetEditors = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{
				.Type = DAssetEditorSubsystem::StaticClass(), .Provider = FModuleStartup::GetModuleName()});
		}
		auto CanShutdownModule() const -> bool override
		{ return !IsGarbageCollecting() && (!GEditor || (!GEditor->IsShutdownRequested() && !GEditor->AreEditorSubsystemsUpdating())); }
		auto ShutdownModule() -> void override
		{
			if (GEditor) GEditor->RemoveEditorSubsystems(Provider);
			AssetEditors.reset(); Tools.reset(); Notifications.reset(); Actors.reset();
			FTexturePreview::ReleaseSharedResources();
		}
	private:
		FName Provider;
		std::unique_ptr<FEditorSubsystemRegistration> Actors;
		std::unique_ptr<FEditorSubsystemRegistration> Notifications;
		std::unique_ptr<FEditorSubsystemRegistration> Tools;
		std::unique_ptr<FEditorSubsystemRegistration> AssetEditors;
	};
	IMPLEMENT_MODULE(FDurinEdModule, DurinEd)
}
