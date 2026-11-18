#include "Widgets/MMaterialParameterCollectionEditor.h"
#include "Misc/UnifiedError.h"
#include "PropertyEditor/DetailPanel.h"

#include "Asset/Asset.h"
#include "DObject/Package.h"
#include "Editor/EditorEngine.h"
#include "Materials/MaterialParameterCollection.h"
#include "ThirdParty/ImGui/imgui.h"
#include "Workspace/MaterialEditorWorkspace.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace Durin::MaterialEditor
{
	auto MMaterialParameterCollectionEditor::PostInitAssetEditor() -> void
	{
		FBaseAssetToolkit::PostInitAssetEditor();
		Session.BindEditor(GetAssetEditorSubsystem()->GetEditor());
	}

	auto MMaterialParameterCollectionEditor::GetToolkitId() const -> const FAssetEditorToolkitId&
	{
		return Workspace::CollectionType;
	}

	auto MMaterialParameterCollectionEditor::OnOpenAsset(DObject& Object) -> bool
	{
		return Cast<DMaterialParameterCollection>(&Object) != nullptr;
	}

	auto MMaterialParameterCollectionEditor::OnActivateAsset(DObject& Object) -> void
	{
		if (!IsReadOnly()) Session.Activate(&Object);
	}

	auto MMaterialParameterCollectionEditor::RequestDeactivate() -> bool
	{
		return FinishEdit(true);
	}

	auto MMaterialParameterCollectionEditor::OnClose() -> void
	{
		FinishEdit(true);
		FBaseAssetToolkit::OnClose();
	}

	auto MMaterialParameterCollectionEditor::OnSaveAsset(DObject& Object) -> bool
	{
		return FinishEdit(false) && Session.Save(&Object, {},
			[this](std::string Message) { Report(std::move(Message)); });
	}

	auto MMaterialParameterCollectionEditor::OnDiscardAsset(DObject& Object) -> bool
	{
		return FinishEdit(true) && Session.Discard(&Object, {},
			[this](std::string Message) { Report(std::move(Message)); });
	}

	auto MMaterialParameterCollectionEditor::OnPackageReloaded(DPackage*, DPackage*) -> void
	{
		if (auto* Object = GetActiveAsset()) OnActivateAsset(*Object);
	}

	auto MMaterialParameterCollectionEditor::OnIsAssetDirty(const DObject& Object) const -> bool
	{
		return Session.IsDirty(&Object);
	}

	auto MMaterialParameterCollectionEditor::OnCanSaveActiveDocument() const -> bool
	{
		return Session.CanSave(GetActiveAsset());
	}

	auto MMaterialParameterCollectionEditor::OnDrawToolkit(bool bActive) -> bool
	{
		if (!bActive && PropertyView.IsEditing()) FinishEdit(true);
		return DrawAssetTabs(Workspace::CollectionRootKey, [this](DObject& Object) {
				auto* Collection = Cast<DMaterialParameterCollection>(&Object);
				if (!Collection) return;
				ImGui::TextDisabled("Up to %u numeric declarations; names are case-insensitively unique.",
					MaterialParameterCollectionMaxDeclarationCount);
				const auto ViewContext = Context();
				const auto Result = DrawDetailPanel(PropertyView, ViewContext, Collection, {
					.PropertyTableId = "MaterialParameterCollectionProperties", .bShowEmptyMessage = false});
				if (!Result.bOwnerReady) return;
				if (Result.bChanged || Result.bRefreshRequested)
					if (const auto Validation = Collection->Validate(); !Validation)
						Report(Validation.error().Message);
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

	auto MMaterialParameterCollectionEditor::FinishEdit(bool bCancel) -> bool
	{
		const auto ViewContext = Context();
		return PropertyView.FinishActiveEdit(&ViewContext, bCancel);
	}

	auto MMaterialParameterCollectionEditor::Context() -> FPropertyViewContext
	{
		return {
			.ReportError = [this](std::string Message) {
				Report(std::move(Message));
			}, .bReadOnly = IsReadOnly()};
	}

	auto MMaterialParameterCollectionEditor::Report(std::string Message) -> void
	{
		ErrorMessage = std::move(Message);
	}
}
