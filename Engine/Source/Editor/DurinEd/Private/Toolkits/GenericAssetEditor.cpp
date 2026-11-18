#include "Toolkits/GenericAssetEditor.h"

#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/AssetEditorSession.h"
#include "PropertyEditor/PropertyView.h"
#include "PropertyEditor/DetailPanel.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Editor/EditorEngine.h"

namespace Durin
{
	class FGenericAssetToolkit final : public FBaseAssetToolkit
	{
	public:
		auto GetLastOperationError() const -> std::string override { return Error; }
		auto GetToolkitId() const -> const FAssetEditorToolkitId& override { return Type; }
		auto RequestDeactivate() -> bool override { const auto Context = ViewContext(); return Properties.FinishActiveEdit(&Context, true); }
		auto ResetLayout() -> void override { ResetPanels(); }
		auto GetUndoDescription() const -> std::string override { return Session.GetUndoDescription(); }
		auto GetRedoDescription() const -> std::string override { return Session.GetRedoDescription(); }
	protected:
		auto PostInitAssetEditor() -> void override
		{
			FBaseAssetToolkit::PostInitAssetEditor();
			Session.BindEditor(GetAssetEditorSubsystem()->GetEditor());
		}
		auto OnClose() -> void override
		{
			RequestDeactivate();
			FBaseAssetToolkit::OnClose();
		}
		auto OnActivateAsset(DObject& Object) -> void override
		{ if (!IsReadOnly()) Session.Activate(&Object); }
		auto OnDrawToolkit(bool bActive) -> bool override
		{
			if (!bActive) RequestDeactivate();
			return DrawAssetTabs("GenericAssetEditor", [](DObject&) {});
		}
		auto CreateDefaultDetailsPanel() -> std::optional<FToolkitPanelSpawner> override
		{ return FToolkitPanelSpawner{"Details", "Details", [this] { DrawDetails(); }}; }
		auto OnSaveAsset(DObject& Object) -> bool override
		{
			const auto Context = ViewContext();
			return Properties.FinishActiveEdit(&Context, false)
				&& Session.Save(&Object, {}, [this](std::string Message) { Error = std::move(Message); });
		}
		auto OnDiscardAsset(DObject& Object) -> bool override
		{
			return RequestDeactivate() && Session.Discard(&Object, {},
				[this](std::string Message) { Error = std::move(Message); });
		}
		auto OnPackageReloaded(DPackage*, DPackage*) -> void override
		{ if (auto* Active = GetActiveAsset()) OnActivateAsset(*Active); }
		auto OnIsAssetDirty(const DObject& Object) const -> bool override { return Session.IsDirty(&Object); }
		auto OnCanSaveActiveDocument() const -> bool override { return Session.CanSave(GetActiveAsset()); }
		auto OnCanUndo() const -> bool override { return Session.CanUndo(); }
		auto OnCanRedo() const -> bool override { return Session.CanRedo(); }
		auto OnUndo() -> bool override { return RequestDeactivate() && Session.Undo(); }
		auto OnRedo() -> bool override { return RequestDeactivate() && Session.Redo(); }
	private:
		auto ViewContext() -> FPropertyViewContext
		{ return {.ReportError = [this](std::string Message) { Error = std::move(Message); }, .bReadOnly = IsReadOnly()}; }
		auto DrawDetails() -> void
		{
			if (auto* Object = GetActiveAsset())
			{
				const auto Context = ViewContext();
				DrawDetailPanel(Properties, Context, Object);
			}
			if (!Error.empty()) ImGui::TextWrapped("%s", Error.c_str());
		}
		FAssetEditorToolkitId Type{"GenericAssetEditor"};
		FAssetEditorSession Session;
		FPropertyView Properties;
		std::string Error;
	};
	DGenericAssetEditor::DGenericAssetEditor(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DGenericAssetEditor::CreateToolkit() -> std::shared_ptr<FAssetEditorToolkit>
	{ return std::make_shared<FGenericAssetToolkit>(); }
}
