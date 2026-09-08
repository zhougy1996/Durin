#pragma once

#include "Editor/EditorSubsystem.h"
#include "Editor/Notification.h"
#include "EditorNotificationSubsystem.gen.h"

namespace Durin
{
	// Owns editor-session notifications; MainFrame drives one update before overlay drawing.
	DCLASS(NoClassDefaultObject)
	class DEditorNotificationSubsystem : public DEditorSubsystem
	{
		GENERATED_BODY()
	public:
		DURINED_API explicit DEditorNotificationSubsystem(const FObjectInitializer& Initializer);
		DURINED_API auto Initialize() -> FSubsystemResult override;
		DURINED_API auto Deinitialize() noexcept -> void override;
		auto GetManager() -> Editor::FNotificationManager& { return *Manager; }
	private:
		std::unique_ptr<Editor::FNotificationManager> Manager;
	};
}
