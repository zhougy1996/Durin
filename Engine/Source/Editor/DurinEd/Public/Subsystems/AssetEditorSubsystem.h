#pragma once

#include "CoreMinimal.h"

#include "Editor/EditorSubsystem.h"
#include "DObject/ObjectKey.h"
#include "DObject/AssetPath.h"
#include "Toolkits/IAssetEditorInstance.h"
#include "AssetEditorSubsystem.gen.h"

namespace Durin::Editor { class FWorkspaceManager; }

namespace Durin
{
	// Tracks asset/editor associations per Editor host, independently of the main frame.
	DCLASS(NoClassDefaultObject)
	class DAssetEditorSubsystem : public DEditorSubsystem
	{
		GENERATED_BODY()
	public:
		DURINED_API explicit DAssetEditorSubsystem(const FObjectInitializer& Initializer);
		DURINED_API auto Deinitialize() noexcept -> void override;
		DURINED_API auto CanOpenEditorForAsset(DObject* Asset, std::string* OutErrorMsg = nullptr) -> bool;
		DURINED_API auto OpenEditorForAsset(DObject* Asset) -> bool;
		DURINED_API auto OpenEditorForAsset(const std::string& AssetPathName) -> void;
		DURINED_API auto OpenEditorForAsset(const FObjectPath& AssetPath) -> void;
		DURINED_API auto OpenEditorForAssets(const std::vector<DObject*>& Assets) -> bool;
		DURINED_API auto OpenEditorsForAssets(const std::vector<std::string>& AssetsToOpen) -> void;
		DURINED_API auto FindEditorForAsset(DObject* Asset, bool bFocusIfOpen) -> IAssetEditorInstance*;
		DURINED_API auto FindEditorsForAsset(DObject* Asset) -> std::vector<IAssetEditorInstance*>;
		DURINED_API auto GetAllEditedAssets() -> std::vector<DObject*>;
		DURINED_API auto GetAllOpenEditors() -> std::vector<IAssetEditorInstance*>;
		DURINED_API auto CloseAllEditorsForAsset(DObject* Asset) -> int32;
		DURINED_API auto CloseOtherEditors(DObject* Asset, IAssetEditorInstance* OnlyEditor) -> void;
		DURINED_API auto CloseAllAssetEditors() -> bool;
		DURINED_API auto NotifyAssetOpened(DObject* Asset, IAssetEditorInstance* Instance) -> void;
		DURINED_API auto NotifyAssetsOpened(const std::vector<DObject*>& Assets, IAssetEditorInstance* Instance) -> void;
		DURINED_API auto NotifyAssetClosed(DObject* Asset, IAssetEditorInstance* Instance) -> void;
		DURINED_API auto NotifyEditorClosed(IAssetEditorInstance* Instance) -> void;
	private:
		auto CanCall() const -> bool;
		auto HasEditor(IAssetEditorInstance* Instance) const -> bool;
		// Non-owning instance registry, as in UE. Instances notify before destruction.
		std::unordered_map<FObjectKey, std::vector<IAssetEditorInstance*>, FObjectKeyHash> OpenedAssets;
		std::unordered_map<IAssetEditorInstance*, std::vector<FObjectKey>> OpenedEditors;
		std::unordered_set<FObjectKey, FObjectKeyHash> OpeningAssets;
		Editor::FWorkspaceManager* WorkspaceManager = nullptr;
		friend class Editor::FWorkspaceManager;
	};
}
