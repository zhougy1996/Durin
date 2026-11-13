#include "Subsystems/AssetEditorSubsystem.h"
#include "Subsystems/AssetEditorOpenSupport.h"
#include "Threading/RunnableThread.h"
#include "CoreGlobals.h"

#include "Toolkits/AssetEditor.h"
#include "Toolkits/AssetEditorIdentity.h"
#include "AssetDefinition/AssetDefinition.h"
#include "AssetDefinition/AssetDefinitionRegistry.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Documents/EditorDocumentManager.h"
#include "Editor/EditorEngine.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Class.h"
#include "Toolkits/ToolkitHost.h"

namespace Durin
{
	namespace
	{
		auto GetSessionOpenSupport(const DAssetEditor& Session, std::span<DObject* const> Assets,
			EAssetOpenMethod Method) -> FAssetOpenSupport
		{
			return GetAssetSelectionOpenSupport(Assets, Method, Session.GetAssetOpenSupport({Method}));
		}
	}
	auto DAssetEditorSubsystem::OwnsAssetEditor(const DAssetEditor& Editor) const -> bool
	{ return CanCall() && Sessions.contains(FObjectKey(&Editor)); }
	auto DAssetEditorSubsystem::DispatchEditorCallback(std::function<void()> Callback) -> void
	{
		auto* Editor = GetEditor();
		if (!IsValid(Editor)) return;
		Editor->DispatchSubsystemCallback([&] {
			FScopedEditorTransactionContext Context(Editor);
			Callback();
		});
	}
	auto DAssetEditorSubsystem::CreateToolkitHost(const FAssetOpenArgs& Args) -> std::shared_ptr<IToolkitHost>
	{
		if (Args.Host) return Args.Host;
		if (ToolkitHostFactory) return ToolkitHostFactory(Args);
		return std::make_shared<FNonWindowToolkitHost>(Args.ToolkitMode);
	}

	auto DAssetEditorSubsystem::OpenAssetEditor(DClass* EditorClass,
		const FAssetEditorInitArgs& Args) -> DAssetEditor*
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!CanCall()) return nullptr;
		if (!CanConstructObjectOfClass(EditorClass, DAssetEditor::StaticClass())) return nullptr;
		auto* Session = Cast<DAssetEditor>(NewObject(EditorClass, GetEditor(), {}));
		return Session ? InitializeAssetEditor(*Session, Args) : nullptr;
	}

	auto DAssetEditorSubsystem::InitializeAssetEditor(DAssetEditor& Session,
		const FAssetEditorInitArgs& Args) -> DAssetEditor*
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!CanCall() || Args.OpenArgs.AssetEditors != this || Session.GetOuter() != GetEditor()
			|| Session.IsTemplateObject() || Session.IsInitialized() || Session.IsClosed()) return nullptr;
		const TStrongObjectPtr<DAssetEditor> KeepAlive(&Session);
		const auto Support = GetSessionOpenSupport(Session, Args.Assets, Args.OpenArgs.OpenMethod);
		const auto Mode = Support.ResolveToolkitMode(Args.OpenArgs.ToolkitMode,
			Args.OpenArgs.Host ? std::optional(Args.OpenArgs.Host->GetToolkitMode()) : std::nullopt);
		if (Support.OpenMethod != Args.OpenArgs.OpenMethod || !Mode || (Args.OpenArgs.bIsRestore && !Session.SupportsRestore())) return nullptr;
		auto Effective = Args; Effective.OpenArgs.ToolkitMode = *Mode;
		DAssetEditor* Result = nullptr;
		DispatchEditorCallback([&] { Result = CreateAssetEditor(Session, Effective); });
		return CanCall() ? Result : nullptr;
	}

	auto DAssetEditorSubsystem::CreateAssetEditor(DAssetEditor& Candidate,
		const FAssetEditorInitArgs& Args) -> DAssetEditor*
	{
		if (!CanCall() || Args.OpenArgs.AssetEditors != this) return nullptr;
		const auto* EditorClass = Candidate.GetClass();
		std::vector<FStrongObjectPtr> RequestedRoots;
		for (auto* Asset : Args.Assets)
		{
			if (!IsValid(Asset)) return nullptr;
			RequestedRoots.emplace_back(Asset);
		}
		const std::string Role = std::string(Args.OpenArgs.GetNormalizedRole());
		for (const auto& Existing : GetOwnedAssetEditors())
		{
			if (Existing->GetClass() != EditorClass || Existing->GetOpenMethod() != Args.OpenArgs.OpenMethod
				|| Existing->GetToolkitMode() != Args.OpenArgs.ToolkitMode || Existing->GetInstanceKey() != Role) continue;
			const auto Objects = Existing->GetObjectsToEdit();
			if (HaveSameEditedAssets(Objects, Args.Assets)) return nullptr;
		}
		if (const auto Previous = GetActiveAssetToolkit())
		{
			if (!Previous->RequestDeactivate()) return nullptr;
		}
		if (!CanCall()) return nullptr;
		auto* Session = &Candidate;
		const TStrongObjectPtr<DAssetEditor> KeepAlive(Session);
		if (!Session->Initialize(Args, DispatchingDefinition) || !CanCall())
		{ Session->bClosed = true; Session->ReleaseToolkit(); MarkObjectHierarchyAsGarbage(Session); return nullptr; }
		Sessions.emplace(FObjectKey(Session), KeepAlive);
		SynchronizeToolkitAssets(*Session->GetToolkit());
		if (!CanCall() || Session->IsClosed() || !Sessions.contains(FObjectKey(Session))) return nullptr;
		Session->FocusWindow();
		return CanCall() && !Session->IsClosed() ? Session : nullptr;
	}


	auto DAssetEditorSubsystem::GetOwnedAssetEditors() const -> std::vector<TStrongObjectPtr<DAssetEditor>>
	{
		std::vector<TStrongObjectPtr<DAssetEditor>> Result;
		for (const auto& [Key, Session] : Sessions) Result.push_back(Session);
		std::ranges::sort(Result, [](const auto& A, const auto& B) {
			if (A->GetLastActivationTime() != B->GetLastActivationTime()) return A->GetLastActivationTime() < B->GetLastActivationTime();
			return std::tuple(A->GetWindowId(), A->GetInstanceKey(), A->GetOpenMethod(), A->GetToolkitMode(), A->GetEditorName().ToString())
				< std::tuple(B->GetWindowId(), B->GetInstanceKey(), B->GetOpenMethod(), B->GetToolkitMode(), B->GetEditorName().ToString());
		});
		return Result;
	}

	auto DAssetEditorSubsystem::FindAssetEditor(DObject* Asset, EAssetOpenMethod Method,
		EToolkitMode Mode, const IToolkitHost* Host) -> DAssetEditor*
	{
		if (!CanCall() || !IsValid(Asset)) return nullptr;
		DAssetEditor* Result = nullptr;
		for (const auto& Session : GetOwnedAssetEditors())
		{
			const auto Objects = Session->GetObjectsToEdit();
			if (Session->IsPrimaryEditor() && !Session->IsClosed() && Session->GetOpenMethod() == Method
				&& Session->GetToolkitMode() == Mode && Session->GetInstanceKey().empty()
				&& (!Host || Session->GetToolkitHost().get() == Host)
				&& std::ranges::find(Objects, Asset) != Objects.end())
				if (!Result || Session->GetLastActivationTime() > Result->GetLastActivationTime()
					|| (Session->GetLastActivationTime() == Result->GetLastActivationTime() && Session->GetWindowId() < Result->GetWindowId())) Result = Session.Get();
		}
		return Result;
	}

	auto DAssetEditorSubsystem::TransferAssetEditor(DAssetEditor& Editor, std::shared_ptr<IToolkitHost> Host) -> bool
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!CanCall() || !Host || !Host->IsAvailable() || !Sessions.contains(FObjectKey(&Editor))) return false;
		const TStrongObjectPtr<DAssetEditor> KeepAlive(&Editor);
		bool Transferred = false;
		DispatchEditorCallback([&] {
			const auto Toolkit = Editor.GetToolkit();
			const auto PreviousHost = Editor.GetToolkitHost();
			const auto Documents = Editor.GetDocumentManager();
			if (!Toolkit || !GetSessionOpenSupport(Editor, Editor.GetObjectsToEdit(), Editor.GetOpenMethod()).ResolveToolkitMode(Host->GetToolkitMode(), Host->GetToolkitMode())
				|| !Documents || !Documents->PrepareForExit()) return;
			if (!CanCall() || Editor.IsClosed()) return;
			if (Host == PreviousHost) { Transferred = true; return; }
			for (const auto& Other : GetOwnedAssetEditors())
			{
				if (Other.Get() == &Editor || Other->GetClass() != Editor.GetClass()
					|| Other->GetOpenMethod() != Editor.GetOpenMethod() || Other->GetToolkitMode() != Host->GetToolkitMode()
					|| Other->GetInstanceKey() != Editor.GetInstanceKey()) continue;
				const auto Assets = Other->GetObjectsToEdit();
				if (HaveSameEditedAssets(Assets, Editor.GetObjectsToEdit())) return;
			}
			if (!Host->AttachToolkit(Toolkit)) return;
			if (!CanCall() || Editor.IsClosed())
			{ Host->DetachToolkit(*Toolkit); return; }
			if (PreviousHost) PreviousHost->DetachToolkit(*Toolkit);
			Toolkit->Host = Host;
			Toolkit->OnHostChanged();
			Transferred = true;
		});
		return Transferred && CanCall();
	}

	auto DAssetEditorSubsystem::GetActiveAssetToolkit() const -> std::shared_ptr<FAssetEditorToolkit>
	{
		const auto Toolkit = ActiveToolkit.lock();
		return Toolkit && !Toolkit->IsRetired() ? Toolkit : nullptr;
	}

	auto DAssetEditorSubsystem::DeactivateAssetEditors() -> bool
	{
		if (const auto Toolkit = GetActiveAssetToolkit(); Toolkit && !Toolkit->RequestDeactivate()) return false;
		ActiveToolkit.reset();
		return true;
	}

	auto DAssetEditorSubsystem::ShutdownAssetEditorSessions() -> void
	{
		if (bStoppingSessions) return;
		struct FStopScope { bool& Flag; explicit FStopScope(bool& Value) : Flag(Value) { Flag = true; } ~FStopScope() { Flag = false; } } StopScope(bStoppingSessions);
		for (const auto& Toolkit : GetAssetToolkits())
		{
			requiref(Toolkit->GetDocumentManager()->PrepareForExit(), "Toolkit operations must settle before host teardown");
			RetireAssetToolkit(*Toolkit);
		}
		ActiveToolkit.reset();
		Sessions.clear();
		RestoreFocus.reset();
	}

	auto DAssetEditorSubsystem::HandleTicker(float /*DeltaSeconds*/) -> void
	{
		if (!CanCall()) return;
		DispatchEditorCallback([&] {
			for (const auto& Toolkit : GetAssetToolkits())
			{
				if (!CanCall()) return;
				if (!OwnsToolkit(*Toolkit) || Toolkit->IsRetired()) continue;
				const auto Documents = Toolkit->GetDocumentManager();
				Documents->RefreshDocumentState();
				if (!CanCall()) return;
				if (!OwnsToolkit(*Toolkit) || Toolkit->IsRetired()) continue;
				if (Toolkit->bCloseRequested && Documents->GetCloseCancellationRevision() != Toolkit->CloseCancellationRevision)
					Toolkit->bCloseRequested = false;
				if (Documents->GetDocuments().empty() || Toolkit->bCloseRequested)
					(void)Toolkit->CloseWindow(Toolkit->bCloseRequested ? Toolkit->CloseReason : EAssetEditorCloseReason::AssetEditorHostClosed);
			}
			if (CanCall()) CompleteRestoreFocus();
		});
	}
}
