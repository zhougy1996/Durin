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
#include "Toolkits/ToolkitManager.h"

namespace Durin
{
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
		auto Host = std::make_shared<FNonWindowToolkitHost>(Args.ToolkitMode);
		FallbackHosts.push_back(Host);
		return Host;
	}

	auto DAssetEditorSubsystem::OpenAssetEditor(DClass* EditorClass,
		const FAssetEditorInitArgs& Args) -> DAssetEditor*
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!CanCall()) return nullptr;
		if (!CanConstructObjectOfClass(EditorClass, DAssetEditor::StaticClass())) return nullptr;
		auto* Editor = Cast<DAssetEditor>(NewObject(EditorClass, GetEditor(), {}));
		return Editor ? InitializeAssetEditor(*Editor, Args) : nullptr;
	}

	auto DAssetEditorSubsystem::InitializeAssetEditor(DAssetEditor& Editor,
		const FAssetEditorInitArgs& Args) -> DAssetEditor*
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!CanCall() || Args.OpenArgs.AssetEditors != this || Editor.GetOuter() != GetEditor()
			|| Editor.IsTemplateObject() || Editor.IsInitialized() || Editor.IsClosed()) return nullptr;
		const TStrongObjectPtr<DAssetEditor> KeepAlive(&Editor);
		const auto Support = GetAssetSelectionOpenSupport(Args.Assets, Args.OpenArgs.OpenMethod,
			Editor.GetAssetOpenSupport({Args.OpenArgs.OpenMethod}));
		const auto Mode = Support.ResolveToolkitMode(Args.OpenArgs.ToolkitMode,
			Args.OpenArgs.Host ? std::optional(Args.OpenArgs.Host->GetToolkitMode()) : std::nullopt);
		if (Support.OpenMethod != Args.OpenArgs.OpenMethod || !Mode || (Args.OpenArgs.bIsRestore && !Editor.SupportsRestore())) return nullptr;
		auto Effective = Args; Effective.OpenArgs.ToolkitMode = *Mode;
		DAssetEditor* Result = nullptr;
		DispatchEditorCallback([&] { Result = CreateAssetEditor(Editor, Effective); });
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
		for (const auto& ExistingToolkit : FToolkitManager::Get().GetToolkits(this))
		{
			if (!MatchesAssetEditorIdentity(*ExistingToolkit, Args.OpenArgs, EditorClass)) continue;
			const auto Objects = ExistingToolkit->GetObjectsToEdit();
			if (HaveSameEditedAssets(Objects, Args.Assets)) return nullptr;
		}
		if (const auto Previous = GetActiveAssetToolkit())
		{
			if (!Previous->RequestDeactivate()) return nullptr;
		}
		if (!CanCall()) return nullptr;
		const TStrongObjectPtr<DAssetEditor> KeepAlive(&Candidate);
		const auto Toolkit = Candidate.Initialize(Args, DispatchingDefinition);
		if (!Toolkit || !CanCall() || !FToolkitManager::Get().RegisterNewToolkit(Toolkit))
		{ Candidate.bClosed = true; Candidate.ReleaseToolkit(); MarkObjectHierarchyAsGarbage(&Candidate); return nullptr; }
		SynchronizeToolkitAssets(*Toolkit);
		if (!CanCall() || Candidate.IsClosed() || !OwnsToolkit(*Toolkit)) return nullptr;
		Candidate.FocusWindow();
		return CanCall() && !Candidate.IsClosed() ? &Candidate : nullptr;
	}


	auto DAssetEditorSubsystem::GetActiveAssetToolkit() const -> std::shared_ptr<FAssetEditorToolkit>
	{
		const auto Toolkit = ActiveToolkit.lock();
		return Toolkit && !Toolkit->IsClosed() ? Toolkit : nullptr;
	}

	auto DAssetEditorSubsystem::DeactivateAssetEditors() -> bool
	{
		if (const auto Toolkit = GetActiveAssetToolkit(); Toolkit && !Toolkit->RequestDeactivate()) return false;
		ActiveToolkit.reset();
		return true;
	}

	auto DAssetEditorSubsystem::CloseAssetEditorsForShutdown() -> void
	{
		if (bClosingEditors) return;
		struct FStopScope { bool& Flag; explicit FStopScope(bool& Value) : Flag(Value) { Flag = true; } ~FStopScope() { Flag = false; } } StopScope(bClosingEditors);
		for (const auto& Toolkit : FToolkitManager::Get().GetToolkits(this))
		{
			requiref(Toolkit->GetDocumentManager()->PrepareForExit(), "Toolkit operations must settle before host teardown");
			FToolkitManager::Get().CloseToolkit(*Toolkit);
		}
		ActiveToolkit.reset();
		FallbackHosts.clear();
	}

	auto DAssetEditorSubsystem::HandleTicker(float /*DeltaSeconds*/) -> void
	{
		if (!CanCall()) return;
		DispatchEditorCallback([&] {
			for (const auto& Toolkit : FToolkitManager::Get().GetToolkits(this))
			{
				if (!CanCall()) return;
				if (!OwnsToolkit(*Toolkit) || Toolkit->IsClosed()) continue;
				const auto Documents = Toolkit->GetDocumentManager();
				Documents->RefreshDocumentState();
				if (!CanCall()) return;
				if (!OwnsToolkit(*Toolkit) || Toolkit->IsClosed()) continue;
				if (Toolkit->bCloseRequested && Documents->GetCloseCancellationRevision() != Toolkit->CloseCancellationRevision)
					Toolkit->bCloseRequested = false;
				if (Documents->GetDocuments().empty() || Toolkit->bCloseRequested)
					(void)Toolkit->CloseWindow(Toolkit->bCloseRequested ? Toolkit->CloseReason : EAssetEditorCloseReason::AssetEditorHostClosed);
			}
			if (CanCall())
			{
				// Initialization and close callbacks have settled before reclaiming hosts.
				if (!FallbackHosts.empty())
				{
					const auto OpenToolkits = FToolkitManager::Get().GetToolkits();
					std::erase_if(FallbackHosts, [&](const auto& Host) {
						return std::ranges::none_of(OpenToolkits, [&](const auto& Toolkit) { return Toolkit->GetToolkitHost() == Host; });
					});
				}
			}
		});
	}
}
