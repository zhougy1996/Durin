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

namespace Durin { class DAssetEditor; class FAssetEditorToolkit; struct FAssetEditorInitArgs; struct FPackageReloadRequest; struct FAssetData; }
namespace Durin::MainFrame { struct FMainFrameContext; }

namespace Durin
{
	enum class EAssetEditorCloseResult : uint8;
	enum class EAssetEditorCloseResponse : uint8;

	struct FAssetEditorClosePrompt
	{
		uint64 RequestId = 0;
		std::string WindowId;
		std::string Label;
	};

	// Tracks asset/editor associations per Editor host, independently of the main frame.
	// Business calls run on the game thread. Callbacks may request shutdown/GC, but
	// must not drive host updates, teardown, collection or provider unload themselves.
	DCLASS()
	class DAssetEditorSubsystem : public DEditorSubsystem
	{
		GENERATED_BODY()
	public:
		DURINED_API explicit DAssetEditorSubsystem(const FObjectInitializer& Initializer);
		DURINED_API auto Initialize(FSubsystemCollection& Collection) -> void override;
		DURINED_API auto Deinitialize() noexcept -> void override;
		DURINED_API auto CanOpenEditorForAsset(DObject* Asset, std::string* OutErrorMsg = nullptr) -> bool;
		DURINED_API auto OpenEditorForAsset(DObject* Asset) -> bool;
		DURINED_API auto CanOpenAsset(const FAssetOpenTarget& Target,
			EAssetOpenMethod Method = EAssetOpenMethod::Edit) -> bool;
		DURINED_API auto OpenAssets(const FAssetOpenArgs& Args) -> bool;
		DURINED_API auto IsAssetOpen(const FSoftObjectPath& Path) const -> bool;
		// Publishes to ToolkitManager only after all initial documents and host attachment succeed.
		DURINED_API auto OpenAssetEditor(DClass* EditorClass, const FAssetEditorInitArgs& Args) -> DAssetEditor*;
		// Publishes a caller-configured editor only after successful initialization.
		DURINED_API auto InitializeAssetEditor(DAssetEditor& Editor, const FAssetEditorInitArgs& Args) -> DAssetEditor*;
		// A shell installs its presentation factory; headless hosts use the semantic fallback.
		auto SetToolkitHostFactory(std::function<std::shared_ptr<IToolkitHost>(const FAssetOpenArgs&)> Factory) -> void
		{ ToolkitHostFactory = std::move(Factory); }
		// Both reflected and direct toolkits are owned exclusively by ToolkitManager.
		// Direct admission supports read-only instances only.
		// Mutable-feature Toolkits require View; writable Edit is rejected before host/document work.
		DURINED_API auto InitializeDirectToolkit(std::shared_ptr<FAssetEditorToolkit> Toolkit,
			const FAssetOpenArgs& Args,
			std::span<DObject* const> Assets) -> bool;
		DURINED_API auto ActivateAssetToolkit(FAssetEditorToolkit& Toolkit) -> bool;
		DURINED_API auto GetActiveAssetToolkit() const -> std::shared_ptr<FAssetEditorToolkit>;
		DURINED_API auto DeactivateAssetEditors() -> bool;
		// Subscribers may withdraw subscriptions or close other editors. They must not
		// recursively drive host updates or mutate a document operation's active inputs.
		DURINED_API auto Subscribe(std::function<void(const FAssetEditorEvent&)> Callback) -> FAssetEditorSubscription;
		DURINED_API auto GetRecentAssets() const -> std::vector<FAssetOpenTarget>;
		DURINED_API auto CaptureEditorState() const -> std::vector<FAssetEditorRestoreEntry>;
		DURINED_API auto RestoreEditorState(std::span<const FAssetEditorRestoreEntry> Entries) -> bool;
		DURINED_API auto SaveEditorState(std::string FilePath = {}) const -> bool;
		DURINED_API auto LoadEditorState(std::string FilePath = {}) -> bool;
		DURINED_API auto FindEditorForAsset(DObject* Asset, bool bFocusIfOpen) -> IAssetEditorInstance*;
		DURINED_API auto FindEditorsForAsset(DObject* Asset) -> std::vector<IAssetEditorInstance*>;
		// Registers one participation set for the affected open Toolkits and transaction history.
		DURINED_API auto AddPackageReloadParticipants(FPackageReloadRequest& Request) -> bool;
		DURINED_API auto GetAllEditedAssets() -> std::vector<DObject*>;
		DURINED_API auto GetAllOpenEditors() -> std::vector<IAssetEditorInstance*>;
		DURINED_API auto GetPendingClosePrompt() const -> std::optional<FAssetEditorClosePrompt>;
		DURINED_API auto ResolvePendingClose(uint64 RequestId, EAssetEditorCloseResponse Response) -> EAssetEditorCloseResult;
		DURINED_API auto CloseAllEditorsForAsset(DObject* Asset) -> int32;
		DURINED_API auto CloseOtherEditors(DObject* Asset, IAssetEditorInstance* OnlyEditor) -> void;
		DURINED_API auto CloseAllAssetEditors() -> bool;
		DURINED_API auto NotifyAssetOpened(DObject* Asset, IAssetEditorInstance* Instance) -> void;
		DURINED_API auto NotifyAssetClosed(DObject* Asset, IAssetEditorInstance* Instance) -> void;
		DURINED_API auto NotifyEditorClosed(IAssetEditorInstance* Instance) -> void;
	private:
		friend class DAssetEditor;
		friend class DEditorEngine;
		friend class FAssetEditorToolkit;
		friend class FBaseAssetToolkit;
		friend class FToolkitManager;
		friend struct MainFrame::FMainFrameContext;
		auto CreateToolkitHost(const FAssetOpenArgs& Args) -> std::shared_ptr<IToolkitHost>;
		auto OwnsToolkit(const FAssetEditorToolkit& Toolkit) const -> bool;
		// Internal admission and transaction context, not a reentrancy/lifetime guard.
		DURINED_API auto DispatchEditorCallback(std::function<void()> Callback) -> void;
		// EditorEngine alone advances once per admitted frame. Callbacks must not reenter
		// the update; shutdown stops at completed refresh/close operation boundaries.
		DURINED_API auto HandleTicker(float DeltaSeconds) -> void;
		// Lifecycle owners only, after callbacks and exit/save policy have settled.
		// Deinitialize and shell teardown share this to preserve UI/provider release order.
		DURINED_API auto CloseAssetEditorsForShutdown(bool bExitApproved = false) -> void;
		DURINED_API auto PublishEvent(FAssetEditorEvent Event) -> void;
		auto SynchronizeToolkitAssets(FAssetEditorToolkit& Toolkit, std::vector<FAssetEditorEvent>* Events = nullptr) -> void;
		auto AddAssetAssociation(DObject* Asset, IAssetEditorInstance* Instance, std::vector<FAssetEditorEvent>* Events) -> void;
		auto RemoveAssetAssociation(const FObjectKey& Key, IAssetEditorInstance* Instance, std::vector<FAssetEditorEvent>* Events) -> void;
		auto FinishToolkitClose(FAssetEditorToolkit& Toolkit, DAssetEditor* Editor,
			IAssetEditorInstance* Instance) -> void;
		struct FPendingClose
		{
			FAssetEditorClosePrompt Prompt;
			std::weak_ptr<FAssetEditorToolkit> Toolkit;
			std::optional<EAssetEditorCloseReason> WindowReason;
			FObjectKey Asset;
			bool bResolving = false;
			bool bResume = false;
		};
		auto FindPendingClose(const FAssetEditorToolkit& Toolkit) const -> std::shared_ptr<FPendingClose>;
		auto QueueClose(FAssetEditorToolkit& Toolkit, FObjectKey Asset, std::string Label) -> void;
		auto ResumeReadyCloseRequests() -> void;
		std::deque<std::shared_ptr<FPendingClose>> PendingCloses;
		uint64 NextCloseRequest = 1;

		auto OnAssetRelocated(const FAssetData& Asset, const FSoftObjectPath& Previous) -> void;
		auto CanCall() const -> bool;
		auto HasEditor(IAssetEditorInstance* Instance) const -> bool;
		auto CreateAssetEditor(DAssetEditor& Editor, const FAssetEditorInitArgs& Args) -> DAssetEditor*;
		std::unordered_set<FSoftObjectPath> OpeningPaths;
		FStrongObjectPtr DispatchingDefinition;
		std::weak_ptr<FAssetEditorToolkit> ActiveToolkit;
		// A non-window shell owns only its presentation hosts, never toolkits/editors.
		std::vector<std::shared_ptr<IToolkitHost>> FallbackHosts;
		std::function<std::shared_ptr<IToolkitHost>(const FAssetOpenArgs&)> ToolkitHostFactory;
		FDelegateHandle AssetRelocationHandle;
		bool bClosingEditors = false;
		struct FLifecycleState;
		std::shared_ptr<FLifecycleState> Lifecycle;
		// Non-owning instance registry, as in UE. Instances notify before destruction.
		std::unordered_map<FObjectKey, std::vector<IAssetEditorInstance*>, FObjectKeyHash> OpenedAssets;
		std::unordered_map<IAssetEditorInstance*, std::vector<FObjectKey>> OpenedEditors;
		std::unordered_set<FObjectKey, FObjectKeyHash> OpeningAssets;
	};
}
