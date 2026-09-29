#include "Widgets/MMaterialParameterCollectionEditor.h"

#include "Asset/Asset.h"
#include "DObject/Package.h"
#include "Editor/EditorEngine.h"
#include "Materials/MaterialParameterCollection.h"
#include "ThirdParty/ImGui/imgui.h"
#include "Workspace/MaterialEditorWorkspace.h"
#include "Workspace/WorkspaceManager.h"

namespace Durin::Editor::Material
{
	auto MMaterialParameterCollectionEditor::GetWorkspaceType() const
		-> const FWorkspaceTypeId&
	{
		return Workspace::CollectionType;
	}

	auto MMaterialParameterCollectionEditor::OpenDocument(
		const FDocumentTab& Document) -> EDocumentOpenResult
	{
		if (Document.ResourceId.empty()) return EDocumentOpenResult::Rejected;
		if (Find(Document.ResourceId)) return EDocumentOpenResult::Opened;
		FObjectPath Path;
		if (const auto Parsed = FObjectPath::TryCreateWithDiagnostic(
			Document.ResourceId, Path); !Parsed)
		{
			Report(ToString(Parsed.error()));
			return EDocumentOpenResult::Rejected;
		}
		auto Loaded = LoadObject<DMaterialParameterCollection>(Path);
		if (!Loaded)
		{
			Report(Loaded.error().Message);
			return EDocumentOpenResult::Rejected;
		}
		Collections.emplace(Document.ResourceId, *Loaded);
		return EDocumentOpenResult::Opened;
	}

	auto MMaterialParameterCollectionEditor::ActivateDocument(
		const FDocumentTab& Document) -> void
	{
		auto* Collection = Find(Document.ResourceId);
		if (PropertyView.IsEditing() && !PropertyView.IsEditingObject(Collection))
			if (!FinishEdit(true)) return;
		Documents.Activate(Document, Collection);
	}

	auto MMaterialParameterCollectionEditor::RequestDeactivate() -> bool
	{
		return FinishEdit(true);
	}

	auto MMaterialParameterCollectionEditor::RequestCloseDocument(
		const FDocumentTab& Document) -> EDocumentCloseResult
	{
		if (PropertyView.IsEditingObject(Find(Document.ResourceId))
			&& !FinishEdit(true)) return EDocumentCloseResult::Rejected;
		if (IsDocumentDirty(Document))
			return EDocumentCloseResult::PendingConfirmation;
		Collections.erase(Document.ResourceId);
		Documents.Close(Document.ResourceId);
		return EDocumentCloseResult::Closed;
	}

	auto MMaterialParameterCollectionEditor::SaveDocument(
		const FDocumentTab& Document) -> bool
	{
		if (!FinishEdit(false)) return false;
		return Documents.Save(Find(Document.ResourceId), {},
			[this](std::string Message) { Report(std::move(Message)); });
	}

	auto MMaterialParameterCollectionEditor::DiscardDocument(
		const FDocumentTab& Document) -> bool
	{
		auto* Collection = Find(Document.ResourceId);
		if (!Collection || !FinishEdit(true)) return false;
		return Documents.Discard(Collection, {},
			[this](DPackage* Previous, DPackage* Replacement) {
				Manager.NotifyPackageReloaded(Previous, Replacement);
			}, [this](std::string Message) { Report(std::move(Message)); });
	}

	auto MMaterialParameterCollectionEditor::OnPackageReloaded(
		DPackage* Previous, DPackage* Replacement) -> void
	{
		FinishEdit(true);
		for (auto& [ResourceId, Collection] : Collections)
			if (Collection && Collection->GetPackage() == Previous)
				Collection = Cast<DMaterialParameterCollection>(
					Replacement->FindTopLevelAsset(Collection->GetFName()));
	}

	auto MMaterialParameterCollectionEditor::IsDocumentDirty(
		const FDocumentTab& Document) const -> bool
	{
		return Documents.IsDirty(Find(Document.ResourceId));
	}

	auto MMaterialParameterCollectionEditor::CanSaveActiveDocument() const -> bool
	{
		return Documents.CanSave(Active());
	}

	auto MMaterialParameterCollectionEditor::SaveActiveDocument() -> bool
	{
		const auto Resource = std::string(Documents.GetActiveResourceId());
		if (Resource.empty()) return false;
		if (!FinishEdit(false)) return false;
		return Documents.Save(Find(Resource), {},
			[this](std::string Message) { Report(std::move(Message)); });
	}

	auto MMaterialParameterCollectionEditor::DrawWorkspace(bool bActive) -> bool
	{
		if (!bActive && PropertyView.IsEditing()) FinishEdit(true);
		return Documents.GetDocumentHost().DrawDocuments(
			Manager, Workspace::CollectionType, Workspace::CollectionRootKey,
			[this](const FDocumentTab& Document) {
				return Find(Document.ResourceId) != nullptr;
			}, [this](const FDocumentTab& Document) {
				auto* Collection = Find(Document.ResourceId);
				if (!Collection) return;
				ImGui::TextDisabled("Up to %u numeric declarations; names are case-insensitively unique.",
					MaterialParameterCollectionMaxDeclarationCount);
				const auto Result = PropertyView.EditObject(Context(), Collection,
					{.PropertyTableId = "MaterialParameterCollectionProperties"});
				if (Result.bChanged && !Collection->Validate())
					Report(Collection->Validate().Message);
				const auto Layout = Collection->BuildLayout();
				ImGui::SeparatorText("Diagnostics");
				if (Layout)
					ImGui::Text("%zu declarations, %u uniform bytes, schema revision %llu, defaults revision %llu",
						Collection->GetDeclarations().size(),
						Layout->UniformLayout.UniformPayloadSize,
						static_cast<unsigned long long>(Collection->GetSchemaRevision()),
						static_cast<unsigned long long>(Collection->GetDefaultsRevision()));
				else ImGui::TextWrapped("%s", Layout.error().Message.c_str());
				if (!ErrorMessage.empty()) ImGui::TextWrapped("%s", ErrorMessage.c_str());
			});
	}

	auto MMaterialParameterCollectionEditor::Find(
		std::string_view ResourceId) const -> DMaterialParameterCollection*
	{
		const auto It = Collections.find(std::string(ResourceId));
		return It == Collections.end() || !It->second.IsValid()
			? nullptr : It->second.Get();
	}

	auto MMaterialParameterCollectionEditor::Active() const
		-> DMaterialParameterCollection*
	{
		return Find(Documents.GetActiveResourceId());
	}

	auto MMaterialParameterCollectionEditor::FinishEdit(bool bCancel) -> bool
	{
		const auto ViewContext = Context();
		return PropertyView.FinishActiveEdit(&ViewContext, bCancel);
	}

	auto MMaterialParameterCollectionEditor::Context() -> FPropertyViewContext
	{
		return {.Transactor = GEditor ? GEditor->GetTransactor() : nullptr,
			.ReportError = [this](std::string Message) {
				Report(std::move(Message));
			}};
	}

	auto MMaterialParameterCollectionEditor::Report(std::string Message) -> void
	{
		ErrorMessage = std::move(Message);
	}
}
