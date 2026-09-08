#include "Editor/EditorNotificationSubsystem.h"
#include "Editor/EditorEngine.h"

namespace Durin
{
	DEditorNotificationSubsystem::DEditorNotificationSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DEditorNotificationSubsystem::Initialize() -> FSubsystemResult
	{
		Manager = std::make_unique<Editor::FNotificationManager>(GetWorkGate(),
			[Host = GetEditor()](std::function<void()> Callback) { Host->DispatchSubsystemCallback(std::move(Callback)); });
		return {};
	}
	auto DEditorNotificationSubsystem::Deinitialize() noexcept -> void
	{
		if (Manager) Manager->Retire();
	}
}
