#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"
#include "DObject/Object.h"
#include "DObject/WeakObjectPtr.h"
#include "DObject/StrongObjectPtr.h"
#include "AssetDefinition/AssetOpenTypes.h"
#include "Toolkits/IAssetEditorInstance.h"
#include "Documents/EditorDocumentTypes.h"
#include "AssetEditor.gen.h"

namespace Durin
{
	class FAssetEditorToolkit;
	class FEditorDocumentManager;

	// Loaded execution inputs stay separate from immutable definition metadata.
	struct FAssetEditorInitArgs
	{
		FAssetOpenArgs OpenArgs;
		std::vector<DObject*> Assets;
		bool bPrimary = true;
		// Stable role distinguishes explicit additional sessions and their saved layouts.
		std::string InstanceKey;
		std::vector<FDocumentRequest> InitialDocuments;
	};

	// Owns one editing session; one session, not each internal document, is indexed externally.
	DCLASS(Abstract, NoClassDefaultObject)
	class DAssetEditor : public DObject, public IAssetEditorInstance
	{
		GENERATED_BODY()
	public:
		DURINED_API explicit DAssetEditor(const FObjectInitializer& Initializer);
		DURINED_API ~DAssetEditor() override;
		DURINED_API auto BeginDestroy() -> void override;
		DURINED_API auto GetEditorName() const -> FName override;
		auto IsPrimaryEditor() const -> bool override { return bPrimary; }
		DURINED_API auto FocusWindow(DObject* Object = nullptr) -> void override;
		auto GetLastActivationTime() -> double override { return LastActivationTime; }
		DURINED_API auto CloseWindow(EAssetEditorCloseReason Reason) -> bool override;
		auto GetLastCloseResult() const -> EDocumentCloseResult { return LastCloseResult; }
		auto GetOpenMethod() const -> EAssetOpenMethod override { return OpenMethod; }
		DURINED_API auto GetToolkitMode() const -> EToolkitMode;
		auto GetToolkit() const -> std::shared_ptr<FAssetEditorToolkit> { return Toolkit; }
		auto GetDocumentManager() const -> std::shared_ptr<FEditorDocumentManager> { return Documents; }
		auto GetToolkitHost() const -> std::shared_ptr<IToolkitHost> { return Host; }
		auto GetInstanceKey() const -> std::string_view { return InstanceKey; }
		auto GetWindowId() const -> std::string_view { return WindowId; }
		DURINED_API auto TransferToHost(std::shared_ptr<IToolkitHost> NewHost) -> bool;
		DURINED_API auto DrawToolkitFrame(bool bActive) -> bool;
		auto GetAssetEditorSubsystem() const -> DAssetEditorSubsystem* { return AssetEditors.Get(); }
		DURINED_API auto GetObjectsToEdit() const -> std::vector<DObject*>;
		DURINED_API auto SynchronizeAssets() -> void;
		auto IsClosed() const -> bool { return bClosed; }
		auto IsInitialized() const -> bool { return bInitialized; }
		virtual auto GetAssetOpenSupport(const FAssetOpenSupportArgs& Args) const -> FAssetOpenSupport { return GetDefaultAssetOpenSupport(Args); }
		virtual auto SupportsRestore() const -> bool { return true; }
	protected:
		virtual auto CreateToolkit() -> std::shared_ptr<FAssetEditorToolkit> { return {}; }
		virtual auto AllowsEmptyAssets() const -> bool { return false; }
	private:
		friend class DAssetEditorSubsystem;
		auto Initialize(const FAssetEditorInitArgs& Args, FStrongObjectPtr Provider) -> bool;
		auto ReleaseToolkit() -> void;
		auto FocusWindowImpl(DObject* Object) -> void;
		auto CloseWindowImpl(EAssetEditorCloseReason Reason) -> bool;
		TWeakObjectPtr<DAssetEditorSubsystem> AssetEditors;
		std::shared_ptr<FEditorDocumentManager> Documents;
		std::shared_ptr<FAssetEditorToolkit> Toolkit;
		std::shared_ptr<IToolkitHost> Host;
		EAssetOpenMethod OpenMethod = EAssetOpenMethod::Edit;
		EDocumentCloseResult LastCloseResult = EDocumentCloseResult::Closed;
		double LastActivationTime = 0;
		bool bPrimary = true;
		bool bInitialized = false;
		bool bClosed = false;
		bool bCloseRequested = false;
		uint64 CloseCancellationRevision = 0;
		EAssetEditorCloseReason CloseReason = EAssetEditorCloseReason::AssetEditorHostClosed;
		std::string InstanceKey;
		std::string WindowId;
	};
}
