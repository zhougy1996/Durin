#include "Notifications/EditorNotificationSubsystem.h"
#include "Editor/EditorEngine.h"

namespace Durin
{
	DEditorNotificationSubsystem::DEditorNotificationSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DEditorNotificationSubsystem::Initialize(FSubsystemCollection& Collection) -> void
	{
		Manager = std::make_unique<Editor::FNotificationManager>(
			[Host = GetEditor()](std::function<void()> Callback) { Host->DispatchSubsystemCallback(std::move(Callback)); });
	}
	auto DEditorNotificationSubsystem::Tick(float DeltaSeconds) -> void
	{
		if (Manager) Manager->Tick(DeltaSeconds);
	}
	auto DEditorNotificationSubsystem::RequestShutdown() noexcept -> void
	{
		if (Manager) Manager->StopAcceptingRequests();
	}
	auto DEditorNotificationSubsystem::Deinitialize() noexcept -> void
	{
		if (Manager) Manager->Retire();
	}
}
