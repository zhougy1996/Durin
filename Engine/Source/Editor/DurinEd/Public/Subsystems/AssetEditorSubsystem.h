#pragma once

#include "CoreMinimal.h"

#include "Editor/EditorSubsystem.h"
#include "DObject/ObjectKey.h"
#include "DObject/AssetPath.h"
#include "Toolkits/IAssetEditorInstance.h"
#include "AssetDefinition/AssetOpenTypes.h"
#include "AssetDefinition/AssetEditorLifecycle.h"
#include "DObject/StrongObjectPtr.h"
#include "AssetEditorSubsystem.gen.h"

namespace Durin { class FEditorDocumentManager; class DAssetEditor; class FAssetEditorToolkit; class FToolkitManager; struct FAssetEditorInitArgs; }

namespace Durin
{
	// Tracks asset/editor associations per Editor host, independently of the main frame.
	DCLASS()
	class DAssetEditorSubsystem : public DEditorSubsystem
	{
		GENERATED_BODY()
	public:
		DURINED_API explicit DAssetEditorSubsystem(const FObjectInitializer& Initializer);
		DURINED_API auto Initialize() -> FSubsystemResult override;
		DURINED_API auto Deinitialize() noexcept -> void override;
		DURINED_API auto CanOpenEditorForAsset(DObject* Asset, std::string* OutErrorMsg = nullptr) -> bool;
		DURINED_API auto OpenEditorForAsset(DObject* Asset) -> bool;
		DURINED_API auto OpenEditorForAsset(const std::string& AssetPathName) -> void;
		DURINED_API auto OpenEditorForAsset(const FObjectPath& AssetPath) -> void;
		DURINED_API auto OpenEditorForAssets(const std::vector<DObject*>& Assets) -> bool;
		DURINED_API auto OpenEditorsForAssets(const std::vector<std::string>& AssetsToOpen) -> void;
		DURINED_API auto CanOpenAsset(const FAssetOpenTarget& Target,
			EAssetOpenMethod Method = EAssetOpenMethod::Edit) -> bool;
		DURINED_API auto OpenAssets(const FAssetOpenArgs& Args) -> FAssetOpenResult;
		// Definition callbacks create pending work while dispatch retains their provider object.
		DURINED_API auto BeginOpenRequest(FAssetOpenTarget Target,
			std::function<bool()> Cancel) -> FAssetOpenRequestId;
		DURINED_API auto CompleteOpenRequest(FAssetOpenRequestId Id,
			EAssetOpenStatus Status, std::string Message = {}) -> bool;
		DURINED_API auto GetOpenRequest(FAssetOpenRequestId Id) const -> std::optional<FAssetOpenItemResult>;
		// A refused cancellation leaves pending work and its provider lifetime intact.
		DURINED_API auto CancelOpenRequest(FAssetOpenRequestId Id) -> bool;
		DURINED_API auto BindOpenRequestToDocument(FAssetOpenRequestId Id, uint64 DocumentId) -> bool;
		// Creates and commits a reflected session only after all initial documents open.
		DURINED_API auto OpenAssetEditor(DClass* EditorClass, const FAssetEditorInitArgs& Args) -> DAssetEditor*;
		// Publishes a caller-configured reflected session only after successful initialization.
		DURINED_API auto InitializeAssetEditor(DAssetEditor& Session, const FAssetEditorInitArgs& Args) -> DAssetEditor*;
		DURINED_API auto FindAssetEditor(DObject* Asset, EAssetOpenMethod Method,
			EToolkitMode Mode = EToolkitMode::Standalone, const IToolkitHost* Host = nullptr) -> DAssetEditor*;
		DURINED_API auto TransferAssetEditor(DAssetEditor& Editor, std::shared_ptr<IToolkitHost> Host) -> bool;
		// A shell installs its presentation factory; headless hosts use the semantic fallback.
		auto SetToolkitHostFactory(std::function<std::shared_ptr<IToolkitHost>(const FAssetOpenArgs&)> Factory) -> void
		{ ToolkitHostFactory = std::move(Factory); }
		DURINED_API auto CreateToolkitHost(const FAssetOpenArgs& Args) -> std::shared_ptr<IToolkitHost>;
		DURINED_API auto DispatchEditorCallback(std::function<void()> Callback) -> void;
		DURINED_API auto OwnsAssetEditor(const DAssetEditor& Editor) const -> bool;
		// Strong snapshots keep a session alive across reentrant frame/command callbacks.
		DURINED_API auto GetOwnedAssetEditors() const -> std::vector<TStrongObjectPtr<DAssetEditor>>;
		DURINED_API auto GetActiveAssetToolkit() const -> std::shared_ptr<FAssetEditorToolkit>;
		DURINED_API auto GetToolkitManager() const -> FToolkitManager&;
		DURINED_API auto ActivateAssetEditor(DAssetEditor& Editor) -> bool;
		DURINED_API auto DeactivateAssetEditors() -> bool;
		DURINED_API auto RetireAssetEditor(DAssetEditor& Editor) -> void;
		DURINED_API auto TickAssetEditors() -> void;
		// Host teardown releases UI/session ownership after its exit/save policy has settled.
		DURINED_API auto ShutdownAssetEditorSessions() -> void;
		DURINED_API auto Subscribe(std::function<void(const FAssetEditorEvent&)> Callback) -> FAssetEditorSubscription;
		DURINED_API auto GetRecentAssets() const -> std::vector<FAssetOpenTarget>;
		DURINED_API auto GetUsage() const -> FAssetEditorUsage;
		DURINED_API auto CaptureSessions() const -> std::vector<FAssetEditorRestoreEntry>;
		DURINED_API auto RestoreSessions(std::span<const FAssetEditorRestoreEntry> Entries) -> FAssetOpenResult;
		DURINED_API auto SaveSessionState(std::string FilePath = {}) const -> bool;
		DURINED_API auto LoadSessionState(std::string FilePath = {}) -> FAssetOpenResult;
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
		friend class DAssetEditor;
		DURINED_API auto PublishEvent(FAssetEditorEvent Event) -> void;
		DURINED_API auto SynchronizeSessionAssets(DAssetEditor& Editor) -> void;
		auto CancelRestoreFocus() -> void { RestoreFocus.Reset(); }
		auto CanCall() const -> bool;
		auto HasEditor(IAssetEditorInstance* Instance) const -> bool;
		auto CreateAssetEditor(DAssetEditor& Session, const FAssetEditorInitArgs& Args) -> DAssetEditor*;
		auto CompleteDocumentOpen(uint64 DocumentId, EAssetOpenStatus Status) -> void;
		auto CompleteRestoreFocus() -> void;
		struct FOpenRequest
		{
			FAssetOpenItemResult Result;
			FStrongObjectPtr Definition;
			FObjectKey ProviderIdentity;
			std::function<bool()> Cancel;
			uint64 DocumentId = 0;
			EAssetOpenMethod Method = EAssetOpenMethod::Edit;
		};
		std::unordered_map<uint64, FOpenRequest> OpenRequests;
		std::unordered_set<FObjectPath> OpeningPaths;
		FStrongObjectPtr DispatchingDefinition;
		EAssetOpenMethod DispatchingOpenMethod = EAssetOpenMethod::Edit;
		uint64 NextOpenRequestId = 1;
		std::unordered_map<FObjectKey, TStrongObjectPtr<DAssetEditor>, FObjectKeyHash> Sessions;
		TWeakObjectPtr<DAssetEditor> ActiveSession;
		TWeakObjectPtr<DAssetEditor> RestoreFocus;
		std::shared_ptr<FToolkitManager> ToolkitManager;
		std::function<std::shared_ptr<IToolkitHost>(const FAssetOpenArgs&)> ToolkitHostFactory;
		bool bStoppingSessions = false;
		struct FLifecycleState;
		std::shared_ptr<FLifecycleState> Lifecycle;
		std::vector<uint64> CompletedRequests;
		// Non-owning instance registry, as in UE. Instances notify before destruction.
		std::unordered_map<FObjectKey, std::vector<IAssetEditorInstance*>, FObjectKeyHash> OpenedAssets;
		std::unordered_map<IAssetEditorInstance*, std::vector<FObjectKey>> OpenedEditors;
		std::unordered_set<FObjectKey, FObjectKeyHash> OpeningAssets;
		friend class FEditorDocumentManager;
	};
}
