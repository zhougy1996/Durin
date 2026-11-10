#pragma once

#include "CoreMinimal.h"

#include "Editor/EditorSubsystem.h"
#include "Notifications/Notification.h"
#include "EditorNotificationSubsystem.gen.h"

namespace Durin
{
	// Owns editor-session notifications; the editor ticks their lifetimes independently of UI.
	DCLASS()
	class DEditorNotificationSubsystem : public DEditorSubsystem
	{
		GENERATED_BODY()
	public:
		DURINED_API explicit DEditorNotificationSubsystem(const FObjectInitializer& Initializer);
		DURINED_API auto Initialize(FSubsystemCollection& Collection) -> void override;
		DURINED_API auto Deinitialize() noexcept -> void override;
		DURINED_API auto Tick(float DeltaSeconds) -> void;
		auto GetManager() -> Editor::FNotificationManager& { return *Manager; }
	private:
		auto RequestShutdown() noexcept -> void;
		std::unique_ptr<Editor::FNotificationManager> Manager;
		friend class DEditorEngine;
	};
}
