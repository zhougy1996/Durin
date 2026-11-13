#include "Toolkits/GenericAssetEditor.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Documents/EditorDocumentManager.h"
#include "Documents/AssetEditorDocumentModels.h"
#include "PropertyEditor/PropertyView.h"
#include "PropertyEditor/DetailPanel.h"
#include "Asset/Load.h"
#include "DObject/Package.h"
#include "DObject/ObjectPtr.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Editor/EditorEngine.h"

namespace Durin
{
	class FGenericAssetToolkit final : public FBaseAssetToolkit
	{
	public:
		auto GetLastOperationError() const -> std::string override { return Error; }
		explicit FGenericAssetToolkit(FEditorDocumentManager& InManager) : Manager(InManager)
		{
			if (auto* Service = Manager.GetAssetEditorSubsystem()) Documents.BindEditor(Service->GetEditor());
		}
		auto GetToolkitId() const -> const FAssetEditorToolkitId& override { return Type; }
		auto OpenDocument(const FEditorDocument& Document) -> EDocumentOpenResult override
		{
			FSoftObjectPath Path;
			if (!FSoftObjectPath::TryCreate(Document.ResourceId, Path)) return EDocumentOpenResult::Rejected;
			const auto Loaded = LoadObjectDetailed<DObject>(Path);
			if (!Loaded || !IsValid(*Loaded) || (*Loaded)->IsTemplateObject()) return EDocumentOpenResult::Rejected;
			Objects.emplace(Document.ResourceId, *Loaded);
			return EDocumentOpenResult::Opened;
		}
		auto ActivateDocument(const FEditorDocument& Document) -> void override
		{
			if (!RequestDeactivate()) return;
			if (IsReadOnly()) Documents.ActivateReadOnly(Document, Find(Document.ResourceId));
			else Documents.Activate(Document, Find(Document.ResourceId));
		}
		auto RequestDeactivate() -> bool override { const auto Context = ViewContext(); return Properties.FinishActiveEdit(&Context, true); }
		auto RequestCloseDocument(const FEditorDocument& Document) -> EDocumentCloseResult override
		{
			if (!RequestDeactivate()) return EDocumentCloseResult::Rejected;
			if (IsDocumentDirty(Document)) return EDocumentCloseResult::PendingConfirmation;
			Objects.erase(Document.ResourceId); Documents.Close(Document.ResourceId);
			return EDocumentCloseResult::Closed;
		}
		auto OnPackageReloaded(DPackage* Previous, DPackage* Replacement) -> void override
		{
			RequestDeactivate();
			for (auto& [Resource, Object] : Objects)
				if (Object && Object->GetPackage() == Previous) Object = Replacement->FindTopLevelAsset(Object->GetFName());
		}
		auto OnResourceIdRemapped(std::string_view Source, std::string_view Destination) -> void override
		{
			if (auto Node = Objects.extract(std::string(Source)); !Node.empty()) { Node.key() = Destination; Objects.insert(std::move(Node)); }
			Documents.RemapResourceId(Source, Destination);
		}
		auto OnDrawToolkit(bool bActive) -> bool override
		{
			if (!bActive) RequestDeactivate();
			return Documents.GetTabManager().DrawDocuments(Manager, Type, GetPresentationRootKey("GenericAssetEditor"),
				[this](const auto& Document) { return Find(Document.ResourceId) != nullptr; }, [](const auto&) {});
		}
		auto ResetLayout() -> void override { ResetPanels(); }
		auto GetUndoDescription() const -> std::string override { return Documents.GetUndoDescription(); }
		auto GetRedoDescription() const -> std::string override { return Documents.GetRedoDescription(); }
	protected:
		auto CreateDefaultDetailsPanel() -> std::optional<FToolkitPanelSpawner> override
		{ return FToolkitPanelSpawner{"Details", "Details", [this] { DrawDetails(); }}; }
		auto OnSaveDocument(const FEditorDocument& Document) -> bool override
		{
			const auto Context = ViewContext();
			return Properties.FinishActiveEdit(&Context, false) && Documents.Save(Find(Document.ResourceId), {}, [this](std::string Message) { Error = std::move(Message); });
		}
		auto OnDiscardDocument(const FEditorDocument& Document) -> bool override
		{
			return RequestDeactivate() && Documents.Discard(Find(Document.ResourceId), {},
				[this](DPackage* Old, DPackage* New) { Manager.NotifyPackageReloaded(Old, New); },
				[this](std::string Message) { Error = std::move(Message); }, &Manager);
		}
		auto OnIsDocumentDirty(const FEditorDocument& Document) const -> bool override { return Documents.IsDirty(Find(Document.ResourceId)); }
		auto OnCanSaveActiveDocument() const -> bool override { return Documents.CanSave(Find(Documents.GetActiveResourceId())); }
		auto OnSaveActiveDocument() -> bool override { const auto* Active = Manager.GetActiveDocument(); return Active && SaveDocument(*Active); }
		auto OnCanUndo() const -> bool override { return Documents.CanUndo(); }
		auto OnCanRedo() const -> bool override { return Documents.CanRedo(); }
		auto OnUndo() -> bool override { return RequestDeactivate() && Documents.Undo(); }
		auto OnRedo() -> bool override { return RequestDeactivate() && Documents.Redo(); }
	private:
		auto Find(std::string_view Path) const -> DObject*
		{ const auto It = Objects.find(std::string(Path)); return It == Objects.end() ? nullptr : It->second.Get(); }
		auto ViewContext() -> FPropertyViewContext
		{ return {.ReportError = [this](std::string Message) { Error = std::move(Message); }, .bReadOnly = IsReadOnly()}; }
		auto DrawDetails() -> void
		{
			if (auto* Object = Find(Documents.GetActiveResourceId()))
			{
				const auto Context = ViewContext();
				DrawDetailPanel(Properties, Context, Object);
			}
			if (!Error.empty()) ImGui::TextWrapped("%s", Error.c_str());
		}
		FAssetEditorToolkitId Type{"GenericAssetEditor"};
		FEditorDocumentManager& Manager;
		FEditableAssetDocumentModel Documents;
		FPropertyView Properties;
		std::unordered_map<std::string, TObjectPtr<DObject>> Objects;
		std::string Error;
	};
	DGenericAssetEditor::DGenericAssetEditor(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DGenericAssetEditor::CreateToolkit() -> std::shared_ptr<FAssetEditorToolkit>
	{ return std::make_shared<FGenericAssetToolkit>(*GetDocumentManager()); }
}
