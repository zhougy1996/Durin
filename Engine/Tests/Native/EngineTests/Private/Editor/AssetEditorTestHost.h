#pragma once

#include "Editor/EditorEngineTestSupport.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditor.h"
#include "DObject/ObjectLifecycle.h"

namespace Durin::Testing
{
	// Test cleanup retires only sessions created by this scope, before feature modules stop.
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
			Service = Editor->GetEditorSubsystem<DAssetEditorSubsystem>();
			if (auto* Owner = Service.Get())
				for (const auto& Session : Owner->GetOwnedAssetEditors()) Existing.emplace(Session.Get());
		}
		~FScopedAssetEditorTestHost() { Shutdown(); }
		auto GetService() const -> DAssetEditorSubsystem* { return Service.Get(); }
		auto Open(std::string_view Path, std::string_view Class, EAssetOpenMethod Method = EAssetOpenMethod::Edit) -> bool
		{
			FSoftObjectPath AssetPath;
			auto* Owner = Service.Get();
			return Owner && FSoftObjectPath::TryCreate(Path, AssetPath) && Owner->OpenAssets({.AssetEditors = Owner,
				.Assets = {{AssetPath, std::string(Class)}}, .OpenMethod = Method}).IsAccepted();
		}
		auto Shutdown() -> void
		{
			if (auto* Owner = Service.Get())
				for (const auto& Session : Owner->GetOwnedAssetEditors())
					if (!Existing.contains(FObjectKey(Session.Get()))) Owner->RetireAssetEditor(*Session);
			Service = nullptr;
			if (bOwnEditor && Editor)
			{ Editor->PrepareForShutdown(); MarkObjectHierarchyAsGarbage(Editor.Get()); }
			Editor.Reset();
		}
	private:
		TStrongObjectPtr<DEditorEngine> Editor;
		TWeakObjectPtr<DAssetEditorSubsystem> Service;
		std::unordered_set<FObjectKey, FObjectKeyHash> Existing;
		bool bOwnEditor = false;
	};
}
