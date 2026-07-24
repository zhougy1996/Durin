#include "Widgets/MTextureEditor.h"

#include "AssetSystem.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorWorkspaceUI.h"
#include "MonaImGui.h"
#include "MonaImGuiPropertyTable.h"
#include "PixelFormat.h"
#include "Texture/Texture2D.h"
#include "Workspace/TextureEditorWorkspace.h"

namespace Durin
{
	namespace
	{
		auto FormatDimensions(uint32 Width, uint32 Height) -> std::string
		{
			return std::format("{} x {}", Width, Height);
		}

		auto FormatByteCount(uint64 Bytes) -> std::string
		{
			if (Bytes >= 1024 * 1024) return std::format("{:.2f} MiB", static_cast<double>(Bytes) / (1024.0 * 1024.0));
			if (Bytes >= 1024) return std::format("{:.2f} KiB", static_cast<double>(Bytes) / 1024.0);
			return std::format("{} bytes", Bytes);
		}

		auto DrawInfoRow(const char* Label, std::string_view Value) -> void
		{
			MonaImGui::PropertyEdit::BeginRow(Label, true);
			ImGui::TextUnformatted(Value.data(), Value.data() + Value.size());
			MonaImGui::PropertyEdit::EndRow(true);
		}
	}

	MTextureEditor::MTextureEditor(FEditorWorkspaceManager& InWorkspaceManager)
		: WorkspaceManager(InWorkspaceManager)
	{
	}

	MTextureEditor::~MTextureEditor()
	{
		FinishActivePropertyEdit(true);
	}

	auto MTextureEditor::GetWorkspaceType() const -> const FEditorWorkspaceTypeId&
	{
		return TextureEditorWorkspace::Type;
	}

	auto MTextureEditor::OpenDocument(const FEditorDocumentTab& Document) -> bool
	{
		if (Document.ResourceId.empty()) return false;
		if (FindOpenTexture(Document.ResourceId)) return true;
		FAssetPath AssetPath;
		std::string PathError;
		if (!FAssetPath::TryCreate(Document.ResourceId, AssetPath, &PathError))
		{
			SetError(std::move(PathError));
			return false;
		}
		DTexture2D* Texture = nullptr;
		const Asset::FAssetResult Result = Asset::LoadAsset(AssetPath, Texture);
		if (!Result || !Texture)
		{
			SetError(Result ? "The selected asset is not a Texture2D." : Result.Message);
			return false;
		}
		OpenTextures.emplace(Document.ResourceId, Texture);
		return true;
	}

	auto MTextureEditor::ActivateDocument(const FEditorDocumentTab& Document) -> void
	{
		DTexture2D* Texture = FindOpenTexture(Document.ResourceId);
		if (PropertyView.IsEditing() && !PropertyView.IsEditingObject(Texture) && !FinishActivePropertyEdit(true)) return;
		if (Texture) ActiveResourceId = Document.ResourceId;
		DocumentWindows[Document.Id.Value].RequestFocus();
	}

	auto MTextureEditor::RequestDeactivate() -> bool
	{
		return FinishActivePropertyEdit(true);
	}

	auto MTextureEditor::RequestCloseDocument(const FEditorDocumentTab& Document) -> bool
	{
		if (PropertyView.IsEditingObject(FindOpenTexture(Document.ResourceId)) && !FinishActivePropertyEdit(true)) return false;
		if (IsDocumentDirty(Document)) return false;
		OpenTextures.erase(Document.ResourceId);
		DocumentWindows.erase(Document.Id.Value);
		if (ActiveResourceId == Document.ResourceId) ActiveResourceId.clear();
		return true;
	}

	auto MTextureEditor::IsDocumentDirty(const FEditorDocumentTab& Document) const -> bool
	{
		DTexture2D* Texture = FindOpenTexture(Document.ResourceId);
		return Texture && Texture->GetPackage() && Texture->GetPackage()->IsDirty();
	}

	auto MTextureEditor::CanSaveActiveDocument() const -> bool
	{
		DTexture2D* Texture = GetActiveTexture();
		return Texture && Texture->GetPackage();
	}

	auto MTextureEditor::SaveActiveDocument() -> bool
	{
		return SaveTexture(GetActiveTexture());
	}

	auto MTextureEditor::CanUndo() const -> bool
	{
		return GEditor && GEditor->GetTransactionManager().CanUndo();
	}

	auto MTextureEditor::CanRedo() const -> bool
	{
		return GEditor && GEditor->GetTransactionManager().CanRedo();
	}

	auto MTextureEditor::GetUndoDescription() const -> std::string_view
	{
		return CanUndo() ? GEditor->GetTransactionManager().GetUndoDescription() : std::string_view{};
	}

	auto MTextureEditor::GetRedoDescription() const -> std::string_view
	{
		return CanRedo() ? GEditor->GetTransactionManager().GetRedoDescription() : std::string_view{};
	}

	auto MTextureEditor::Undo() -> bool
	{
		return FinishActivePropertyEdit(false) && CanUndo() && GEditor->GetTransactionManager().Undo();
	}

	auto MTextureEditor::Redo() -> bool
	{
		return FinishActivePropertyEdit(false) && CanRedo() && GEditor->GetTransactionManager().Redo();
	}

	auto MTextureEditor::DrawWorkspace(bool bActive) -> bool
	{
		if (!bActive && PropertyView.IsEditing()) FinishActivePropertyEdit(true);
		bool bWorkspaceActivated = false;
		std::vector<FEditorDocumentId> CloseRequests;
		for (const FEditorDocumentTab& Document : WorkspaceManager.GetDocuments())
		{
			if (Document.WorkspaceType != TextureEditorWorkspace::Type) continue;
			DTexture2D* Texture = FindOpenTexture(Document.ResourceId);
			if (!Texture) continue;
			FEditorWorkspaceRootWindow& RootWindow = DocumentWindows[Document.Id.Value];
			const FEditorWorkspaceRootWindowState WindowState = RootWindow.Begin({
				.DisplayName = Document.Label,
				.RootKey = EditorWorkspaceUI::MakeEditorDocumentRootKey(TextureEditorWorkspace::RootKey, Document.DocumentKey),
				.bDirty = Texture->GetPackage() && Texture->GetPackage()->IsDirty(),
			});
			if (WindowState.bFocused || WindowState.bActivated)
			{
				bWorkspaceActivated = true;
				if (ActiveResourceId != Document.ResourceId) WorkspaceManager.ActivateDocument(Document.Id);
			}
			if (WindowState.bVisible) DrawDocument(Document, Texture);
			RootWindow.End();
			if (WindowState.bCloseRequested) CloseRequests.push_back(Document.Id);
		}
		for (FEditorDocumentId DocumentId : CloseRequests) WorkspaceManager.RequestCloseDocument(DocumentId);
		return bWorkspaceActivated;
	}

	auto MTextureEditor::ResetLayout() -> void
	{
	}

	auto MTextureEditor::FindOpenTexture(std::string_view ResourceId) const -> DTexture2D*
	{
		const auto It = OpenTextures.find(std::string(ResourceId));
		return It == OpenTextures.end() ? nullptr : It->second.Get();
	}

	auto MTextureEditor::GetActiveTexture() const -> DTexture2D*
	{
		return FindOpenTexture(ActiveResourceId);
	}

	auto MTextureEditor::SaveTexture(DTexture2D* Texture) -> bool
	{
		if (!Texture || !Texture->GetPackage()) return false;
		const Asset::FAssetResult Result = Asset::SavePackage(Texture->GetPackage());
		if (!Result)
		{
			SetError(Result.Message);
			return false;
		}
		return true;
	}

	auto MTextureEditor::DrawDocument(const FEditorDocumentTab& Document, DTexture2D* Texture) -> void
	{
		if (ImGui::Button("Save")) SaveTexture(Texture);
		ImGui::Separator();
		ImGui::TextDisabled("Asset");
		ImGui::SameLine();
		ImGui::TextUnformatted(Document.ResourceId.c_str());
		ImGui::TextDisabled("Type");
		ImGui::SameLine();
		ImGui::TextUnformatted(Texture->GetClass()->GetQualifiedName().ToString().c_str());
		ImGui::Spacing();
		DrawSourceData(Texture);
		ImGui::Spacing();
		DrawBuildSettings(Texture);

		if (ActiveResourceId != Document.ResourceId) return;
		if (!ErrorMessage.empty()) ImGui::OpenPopup("Texture Editor Error");
		if (ImGui::BeginPopupModal("Texture Editor Error", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
		{
			ImGui::TextWrapped("%s", ErrorMessage.c_str());
			if (ImGui::Button("OK"))
			{
				ErrorMessage.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	auto MTextureEditor::DrawSourceData(DTexture2D* Texture) -> void
	{
		ImGui::SeparatorText("Source");
		if (!MonaImGui::PropertyEdit::BeginTable("TextureSourceData")) return;
		DrawInfoRow("Source File", Texture->GetSourceFile());
		if (const FTextureSourceData* Source = Texture->GetSourceData())
		{
			DrawInfoRow("Dimensions", FormatDimensions(Source->Width, Source->Height));
			DrawInfoRow("Source Channels", std::format("{}", Source->SourceChannelCount));
			DrawInfoRow("Transparency", Source->bHasTransparency ? "Present" : "Opaque");
			DrawInfoRow("Decoded Format", Source->Format == ETextureSourceFormat::RGBA8 ? "RGBA8" : "Invalid");
		}
		else DrawInfoRow("Status", "Source data unavailable");
		MonaImGui::PropertyEdit::EndTable();
	}

	auto MTextureEditor::DrawBuildSettings(DTexture2D* Texture) -> void
	{
		ImGui::SeparatorText("Build Settings and Platform Data");
		if (!MonaImGui::PropertyEdit::BeginTable("TextureBuildSettings")) return;
		FProperty* UsageProperty = Texture->GetClass()->FindPropertyByName("Usage");
		FProperty* SRGBProperty = Texture->GetClass()->FindPropertyByName("bSRGB");
		if (UsageProperty) PropertyView.EditProperty(MakePropertyViewContext(), Texture, UsageProperty, 0, {.Label = "Usage"});
		else DrawInfoRow("Usage", "Reflection metadata unavailable");
		if (SRGBProperty) PropertyView.EditProperty(MakePropertyViewContext(), Texture, SRGBProperty, 0, {.Label = "sRGB"});
		else DrawInfoRow("sRGB", "Reflection metadata unavailable");

		const FTexturePlatformData* Platform = Texture->GetPlatformData();
		if (Platform && Platform->IsValid())
		{
			uint64 TotalBytes = 0;
			for (const FTexture2DMipData& Mip : Platform->Mips) TotalBytes += Mip.Pixels.size();
			DrawInfoRow("Status", "Ready");
			DrawInfoRow("Pixel Format", GetPixelFormatInfo(Platform->PixelFormat).Name);
			DrawInfoRow("Mip Count", std::format("{}", Platform->Mips.size()));
			DrawInfoRow("Mip Range", std::format("{} to {}", FormatDimensions(Platform->Mips.front().Width, Platform->Mips.front().Height),
				FormatDimensions(Platform->Mips.back().Width, Platform->Mips.back().Height)));
			DrawInfoRow("Platform Bytes", FormatByteCount(TotalBytes));
			DrawInfoRow("Residency", "Fully resident");
		}
		else DrawInfoRow("Status", "Platform data unavailable or invalid");
		DrawInfoRow("Build Revision", std::format("{}", Texture->GetBuildRevision()));
		MonaImGui::PropertyEdit::EndTable();
	}

	auto MTextureEditor::FinishActivePropertyEdit(bool bCancel) -> bool
	{
		const FReflectedPropertyViewContext Context = MakePropertyViewContext();
		return PropertyView.FinishActiveEdit(&Context, bCancel);
	}

	auto MTextureEditor::MakePropertyViewContext() -> FReflectedPropertyViewContext
	{
		return {
			.Transactions = GEditor ? &GEditor->GetTransactionManager() : nullptr,
			.ReportError = [this](std::string Error) { SetError(std::move(Error)); },
		};
	}

	auto MTextureEditor::SetError(std::string Message) -> void
	{
		ErrorMessage = std::move(Message);
		DURIN_ERROR("Texture editor: {}", ErrorMessage);
	}
}
