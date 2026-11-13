#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"
#include "DObject/Object.h"
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
		std::vector<FDocumentRequest> InitialDocuments;
	};

	// Optional reflected lifetime owner; runtime state belongs to its toolkit.
	DCLASS(Abstract, NoClassDefaultObject)
	class DAssetEditor : public DObject, public IAssetEditorInstance
	{
		GENERATED_BODY()
	public:
		DURINED_API explicit DAssetEditor(const FObjectInitializer& Initializer);
		DURINED_API ~DAssetEditor() override;
		DURINED_API auto OnToolkitClosed() -> void;
		DURINED_API auto BeginDestroy() -> void override;
		DURINED_API auto GetEditorName() const -> FName override;
		DURINED_API auto IsPrimaryEditor() const -> bool override;
		DURINED_API auto FocusWindow(DObject* Object = nullptr) -> void override;
		DURINED_API auto GetLastActivationTime() -> double override;
		DURINED_API auto CloseWindow(EAssetEditorCloseReason Reason) -> bool override;
		DURINED_API auto GetOpenMethod() const -> EAssetOpenMethod override;
		DURINED_API auto GetToolkitMode() const -> EToolkitMode;
		auto GetToolkit() const -> std::shared_ptr<FAssetEditorToolkit> { return Toolkit; }
		DURINED_API auto GetDocumentManager() const -> std::shared_ptr<FEditorDocumentManager>;
		DURINED_API auto GetToolkitHost() const -> std::shared_ptr<IToolkitHost>;
		DURINED_API auto GetInstanceKey() const -> std::string_view;
		auto GetInstanceId() const -> std::string_view override { return GetWindowId(); }
		DURINED_API auto GetWindowId() const -> std::string_view;
		DURINED_API auto TransferToHost(std::shared_ptr<IToolkitHost> NewHost) -> bool;
		DURINED_API auto GetAssetEditorSubsystem() const -> DAssetEditorSubsystem*;
		DURINED_API auto GetObjectsToEdit() const -> std::vector<DObject*>;
		DURINED_API auto IsClosed() const -> bool;
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
		// Borrowed only while CreateToolkit runs; never retained as runtime state.
		const FAssetEditorInitArgs* InitializingArgs = nullptr;
		std::shared_ptr<FEditorDocumentManager> InitializingDocuments;
		std::shared_ptr<FAssetEditorToolkit> Toolkit;
		bool bInitialized = false;
		bool bClosed = false;
	};
}
