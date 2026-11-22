#include "LevelAssetEditor.h"
#include "FLevelEditorToolkit.h"
#include "LevelEditorModule.h"
#include "Asset/RegistryOperations.h"
#include "Asset/Load.h"
#include "DObject/Package.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "Misc/FeedbackContext.h"
#include "Misc/UnifiedError.h"
#include "Transactions/Transactor.h"
#include "LevelEditorSubsystem.h"
#include "Engine/Level.h"
#include "Editor/EditorEngine.h"
#include "DObject/Class.h"
#include "Icons/FontAwesomeIcons.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace Durin
{
	auto DLevelAssetEditor::CreateToolkit() -> std::shared_ptr<FAssetEditorToolkit>
	{
		if (!HostContext || GetToolkitMode() != EToolkitMode::WorldCentric) return {};
		auto Toolkit = std::make_shared<LevelEditor::FLevelEditorToolkit>(HostContext->Settings,
			HostContext->Browser, HostContext->OpenProjectBrowser, *GetAssetEditorSubsystem()->GetEditor());
		Toolkit->Construct();
		return Toolkit;
	}
	auto DLevelAssetDefinition::GetAssetClassPath() const -> std::string { return DLevel::StaticClass()->GetQualifiedName().ToString(); }
	auto DLevelAssetDefinition::GetAssetIcon() const -> std::string { return Icons::Home; }
	auto DLevelAssetDefinition::PrepareToActivateAssets(const FAssetActivateArgs& Args) const -> std::vector<FAssetOpenTarget>
	{
		auto* Editor = Args.AssetEditors ? Args.AssetEditors->GetEditor() : nullptr;
		auto Targets = Args.Assets;
		if (!Editor || Targets.empty()) return {};
		if (Editor->IsPlaying())
		{
			FFeedbackContext::Get().Error("Stop Play in Editor before opening a level.");
			return {};
		}
		const auto Resolved = ResolveAssetObjectPathForOperation(Targets.front().AssetPath, {.ExpectedClass = DLevel::StaticClass()});
		if (!Resolved)
		{
			FFeedbackContext::Get().Error("The level asset could not be resolved.");
			return {};
		}
		Targets.resize(1);
		Targets.front().AssetPath = Resolved.FinalPath;
		auto* Transactions = Editor->GetTransactor();
		if (Transactions && (!Transactions->CanTransact() || Transactions->IsActive()))
		{
			FFeedbackContext::Get().Error("Finish the current edit or transaction recovery before opening a level.");
			return {};
		}
		FScopedPackageSaveObserver Saved([Transactions](const FPackagePath& Path) {
			if (auto* Package = FindResidentPackage(Path); Package && Transactions && Transactions->GetPackageRevisionState(*Package))
				Transactions->MarkSaved(*Package);
		});
		if (!FEditorFileUtils::SaveDirtyPackages(true, true, true))
		{
			Targets.clear();
			return Targets;
		}
		const auto* Package = FindResidentPackage(Resolved.FinalPath.GetPackagePath());
		if (Package && Package->IsNewlyCreated())
		{
			FFeedbackContext::Get().Error("The level you are trying to open needs to be saved first.");
			return {};
		}
		return Targets;
	}

	auto DLevelAssetDefinition::OpenAssets(const FAssetOpenArgs& Args) const -> EAssetCommandResult
	{
		if (!Args.AssetEditors || Args.bIsRestore || (Args.bCreateSingleEditor && Args.Assets.size() > 1)) return EAssetCommandResult::Unhandled;
		if (Args.Host)
		{
			auto* Owner = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor").GetOrCreateLevelEditor(*Args.AssetEditors);
			if (!Owner || Owner->GetToolkitHost() != Args.Host) return EAssetCommandResult::Unhandled;
		}
		for (const auto& Target : Args.Assets)
		{
			std::optional<UnifiedError::FError> Error;
			auto* Level = LoadObject<DLevel>(Target.AssetPath, nullptr, &Error);
			if (!IsValid(Level) || Level->IsTemplateObject()
				|| Level->GetClass()->GetQualifiedName().ToString() != Target.AssetClassName || !Level->GetPackage())
			{
				if (Error) FFeedbackContext::Get().Error(Error->ToString());
				continue;
			}
			if (Level->GetPackage()->IsNewlyCreated())
			{
				FFeedbackContext::Get().Error("The level you are trying to open needs to be saved first.");
				continue;
			}
			// Copy identity before synchronous loading retires the resident asset graph.
			const std::string MapPath = Level->GetObjectPath();
			FEditorFileUtils::LoadMap(MapPath);
			return EAssetCommandResult::Handled;
		}
		return EAssetCommandResult::Unhandled;
	}
}
