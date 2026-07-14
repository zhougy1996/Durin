#include "LevelEditorModule.h"

#include "EditorSessionSettings.h"
#include "Widgets/MLevelEditor.h"

namespace Durin
{
	IMPLEMENT_MODULE(FLevelEditorModule, LevelEditor)

	FLevelEditorModule::~FLevelEditorModule() = default;

	LEVELEDITOR_API auto FLevelEditorModule::StartupModule() -> void
	{
		SessionSettings = std::make_unique<FEditorSessionSettings>();
		SessionSettings->Load();
	}

	LEVELEDITOR_API auto FLevelEditorModule::ShutdownModule() -> void
	{
		SessionSettings.reset();
	}

	LEVELEDITOR_API auto FLevelEditorModule::CreateLevelEditorWidget() -> std::shared_ptr<MWidget>
	{
		std::shared_ptr<MLevelEditor> LevelEditorWidget = std::make_shared<MLevelEditor>(*SessionSettings);
		LevelEditorWidget->Construct();
		return LevelEditorWidget;
	}

	LEVELEDITOR_API auto FLevelEditorModule::GetWindowWidth() const -> int32
	{
		return SessionSettings->GetWindowWidth();
	}

	LEVELEDITOR_API auto FLevelEditorModule::GetWindowHeight() const -> int32
	{
		return SessionSettings->GetWindowHeight();
	}

	LEVELEDITOR_API auto FLevelEditorModule::GetUIScale() const -> float
	{
		return SessionSettings->GetUIScale();
	}

	LEVELEDITOR_API auto FLevelEditorModule::IsWindowMaximized() const -> bool
	{
		return SessionSettings->IsWindowMaximized();
	}
}
