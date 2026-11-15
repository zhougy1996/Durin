#pragma once

#include "Editor/EditorEngineTestSupport.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/ToolkitManager.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditor.h"
#include "DObject/ObjectLifecycle.h"

namespace Durin::Testing
{
	// Close only toolkits created by this scope, before feature modules stop.
	class FScopedAssetEditorTestHost
	{
	public:
		FScopedAssetEditorTestHost()
		{
			if (GEditor && GEditor->GetEditorSubsystem<DAssetEditorSubsystem>()) Editor = GEditor;
			else
			{
				Editor = NewObject<DEditorEngine>(nullptr, {});
				bOwnEditor = true;
				FEditorEngineTestAccess::StartEditor(*Editor);
			}
			AssetEditorSubsystem = Editor->GetEditorSubsystem<DAssetEditorSubsystem>();
			if (auto* Subsystem = AssetEditorSubsystem.Get())
				for (const auto& Toolkit : FToolkitManager::Get().GetToolkits(Subsystem)) Existing.emplace(Toolkit->GetWindowId());
		}
		~FScopedAssetEditorTestHost() { Shutdown(); }
		auto GetAssetEditorSubsystem() const -> DAssetEditorSubsystem* { return AssetEditorSubsystem.Get(); }
		auto Open(std::string_view Path, std::string_view Class, EAssetOpenMethod Method = EAssetOpenMethod::Edit) -> bool
		{
			FSoftObjectPath AssetPath;
			auto* Subsystem = AssetEditorSubsystem.Get();
			return Subsystem && FSoftObjectPath::TryCreate(Path, AssetPath) && Subsystem->OpenAssets({.AssetEditors = Subsystem,
				.Assets = {{AssetPath, std::string(Class)}}, .OpenMethod = Method});
		}
		auto Shutdown() -> void
		{
			if (auto* Subsystem = AssetEditorSubsystem.Get())
				for (const auto& Toolkit : FToolkitManager::Get().GetToolkits(Subsystem))
					if (!Existing.contains(std::string(Toolkit->GetWindowId()))) FToolkitManager::Get().CloseToolkit(*Toolkit);
			AssetEditorSubsystem = nullptr;
			if (bOwnEditor && Editor)
			{ Editor->PreExit(); MarkObjectHierarchyAsGarbage(Editor.Get()); }
			Editor.Reset();
		}
	private:
		TStrongObjectPtr<DEditorEngine> Editor;
		TWeakObjectPtr<DAssetEditorSubsystem> AssetEditorSubsystem;
		std::unordered_set<std::string> Existing;
		bool bOwnEditor = false;
	};
}
