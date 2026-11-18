#pragma once

#include "CoreMinimal.h"

#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditor.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/ToolkitManager.h"

namespace Durin::Testing
{
	// Reflected-only fixture queries are deliberately outside the production API.
	inline auto GetReflectedEditors(const DAssetEditorSubsystem* Subsystem)
		-> std::vector<TStrongObjectPtr<DAssetEditor>>
	{
		std::vector<TStrongObjectPtr<DAssetEditor>> Result;
		if (!Subsystem) return Result;
		for (const auto& Toolkit : FToolkitManager::Get().GetToolkits(Subsystem))
			if (auto* Editor = Toolkit->GetOwningAssetEditor()) Result.emplace_back(Editor);
		return Result;
	}
	inline auto FindReflectedEditor(const DAssetEditorSubsystem* Subsystem, DObject* Asset,
		EAssetOpenMethod Method, EToolkitMode Mode = EToolkitMode::Standalone,
		const IToolkitHost* Host = nullptr) -> DAssetEditor*
	{
		if (!Subsystem || !Asset) return nullptr;
		DAssetEditor* Result = nullptr;
		for (const auto& Toolkit : FToolkitManager::Get().GetToolkits(Subsystem))
		{
			auto* Editor = Toolkit->GetOwningAssetEditor();
			if (!Editor || !Toolkit->IsEditorOpen() || !Toolkit->IsPrimaryEditor()
				|| Toolkit->GetOpenMethod() != Method || Toolkit->GetToolkitMode() != Mode
				|| !Toolkit->GetInstanceKey().empty() || (Host && Toolkit->GetToolkitHost().get() != Host)) continue;
			const auto Assets = Toolkit->GetEditingObjects();
			if (std::ranges::find(Assets, Asset) != Assets.end()
				&& (!Result || Toolkit->GetLastActivationTime() > Result->GetLastActivationTime()
					|| (Toolkit->GetLastActivationTime() == Result->GetLastActivationTime()
						&& Toolkit->GetWindowId() < Result->GetWindowId()))) Result = Editor;
		}
		return Result;
	}
}
