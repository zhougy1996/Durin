#include "Widgets/MTextureEditor.h"

#include "AssetSystem.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorWorkspaceUI.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"
#include "MonaImGuiPropertyTable.h"
#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "PixelFormat.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DRenderResource.h"
#include "Widgets/TexturePreview.h"
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

		auto GetEnumValueDisplayName(const char* QualifiedEnumName, uint64 Value) -> std::string
		{
			if (const DEnum* Enum = FindEnumByQualifiedName(QualifiedEnumName))
			{
				FName Name;
				if (Enum->FindNameByValue(Value, Name)) return StringUtils::HumanizeName(Name.ToString());
			}
			return std::format("Unknown ({})", Value);
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
		PreviewStates.try_emplace(Document.ResourceId);
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
		if (IsDocumentDirty(Document))
		{
			PendingCloseDocumentId = Document.Id;
			return false;
		}
		OpenTextures.erase(Document.ResourceId);
		PreviewStates.erase(Document.ResourceId);
		DocumentWindows.erase(Document.Id.Value);
		if (ActiveResourceId == Document.ResourceId)
		{
			ActiveResourceId.clear();
		}
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

		if (PendingCloseDocumentId.IsValid())
		{
			const FEditorDocumentTab* PendingDocument = [&]() -> const FEditorDocumentTab* {
				for (const FEditorDocumentTab& Doc : WorkspaceManager.GetDocuments())
					if (Doc.Id == PendingCloseDocumentId) return &Doc;
				return nullptr;
			}();
			if (!PendingDocument)
			{
				PendingCloseDocumentId = {};
				return bWorkspaceActivated;
			}
			ImGui::OpenPopup("ConfirmClose");
			if (ImGui::BeginPopupModal("ConfirmClose", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
			{
				ImGui::TextWrapped("Save changes to \"%s\" before closing?", PendingDocument->Label.c_str());
				ImGui::Spacing();
				if (ImGui::Button("Save", ImVec2(100, 0)))
				{
					DTexture2D* Texture = FindOpenTexture(PendingDocument->ResourceId);
					if (SaveTexture(Texture))
					{
						ImGui::CloseCurrentPopup();
						WorkspaceManager.RequestCloseDocument(PendingCloseDocumentId);
						PendingCloseDocumentId = {};
					}
				}
				ImGui::SameLine();
				if (ImGui::Button("Discard", ImVec2(100, 0)))
				{
					if (DTexture2D* Texture = FindOpenTexture(PendingDocument->ResourceId))
						if (Texture->GetPackage()) Texture->GetPackage()->ClearDirty();
					ImGui::CloseCurrentPopup();
					WorkspaceManager.RequestCloseDocument(PendingCloseDocumentId);
					PendingCloseDocumentId = {};
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel", ImVec2(100, 0)))
				{
					ImGui::CloseCurrentPopup();
					PendingCloseDocumentId = {};
				}
				ImGui::EndPopup();
			}
		}

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
		Texture->RefreshBuildStatus();

		if (ImGui::Button("Save")) SaveTexture(Texture);
		ImGui::Separator();
		ImGui::TextDisabled("Asset");
		ImGui::SameLine();
		ImGui::TextUnformatted(Document.ResourceId.c_str());
		ImGui::TextDisabled("Type");
		ImGui::SameLine();
		ImGui::TextUnformatted(Texture->GetClass()->GetQualifiedName().ToString().c_str());
		ImGui::Spacing();

		DrawFailureState(Texture);
		DrawPreview(Document.ResourceId, Texture);
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

	auto MTextureEditor::DrawFailureState(DTexture2D* Texture) -> void
	{
		const ETextureBuildStatus Status = Texture->GetBuildStatus();
		if (Status == ETextureBuildStatus::Ready || Status == ETextureBuildStatus::Unbuilt)
			return;

		const char* Title = "Build Error";
		ImVec4 TitleColor(1.0f, 0.5f, 0.3f, 1.0f); // Amber default
		std::string Message;

		switch (Status)
		{
		case ETextureBuildStatus::MissingSource:
			Title = "Missing Source";
			TitleColor = ImVec4(1.0f, 0.5f, 0.3f, 1.0f);
			Message = std::format("The source file could not be found:\n{}", Texture->GetLastBuildError());
			break;
		case ETextureBuildStatus::DecodeFailure:
			Title = "Decode Failure";
			TitleColor = ImVec4(1.0f, 0.5f, 0.3f, 1.0f);
			Message = std::format("The source image could not be decoded:\n{}", Texture->GetLastBuildError());
			break;
		case ETextureBuildStatus::BuildFailure:
			Title = "Build Failure";
			TitleColor = ImVec4(1.0f, 0.5f, 0.3f, 1.0f);
			Message = std::format("The platform texture data could not be built:\n{}", Texture->GetLastBuildError());
			break;
		case ETextureBuildStatus::UploadFailure:
			Title = "GPU Upload Failure";
			TitleColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
			Message = "The texture could not be uploaded to the GPU.";
			if (!Texture->GetLastBuildError().empty())
				Message += std::format("\n{}", Texture->GetLastBuildError());
			break;
		case ETextureBuildStatus::UnsupportedFormat:
			Title = "Unsupported Format";
			TitleColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
			Message = "The selected pixel format is not supported by this GPU.";
			if (!Texture->GetLastBuildError().empty())
				Message += std::format("\n{}", Texture->GetLastBuildError());
			break;
		default:
			break;
		}

		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.18f, 0.08f, 0.08f, 0.5f));
		ImGui::BeginChild("TextureFailureState", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
		ImGui::TextColored(TitleColor, "%s", Title);
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.9f, 0.85f, 1.0f));
		ImGui::TextWrapped("%s", Message.c_str());
		ImGui::PopStyleColor();

		ImGui::Spacing();
		if (ImGui::Button("Retry Build"))
		{
			std::string Error;
			if (!Texture->PostLoad(Error))
			{
				SetError(Error);
			}
		}

		if (Status == ETextureBuildStatus::UploadFailure && Texture->GetRenderResource())
		{
			ImGui::SameLine();
			const ERenderResourceState RState = Texture->GetRenderResource()->GetResourceState();
			const std::string StateName = GetEnumValueDisplayName("Durin::ERenderResourceState", static_cast<uint64>(RState));
			ImGui::TextDisabled("(GPU state: %s)", StateName.c_str());
		}

		ImGui::EndChild();
		ImGui::PopStyleColor();
		ImGui::Spacing();
	}

	auto MTextureEditor::DrawPreview(const std::string& ResourceId, DTexture2D* Texture) -> void
	{
		ImGui::SeparatorText("Preview");

		FTexturePreviewState& PreviewState = PreviewStates.try_emplace(ResourceId).first->second;
		FTexturePreview& Preview = *PreviewState.Preview;

		const FTexturePlatformData* Platform = Texture->GetPlatformData();
		const FTextureSourceData* Source = Texture->GetSourceData();

		const bool bRevisionChanged = Texture->GetBuildRevision() != PreviewState.LastObservedRevision;
		if (bRevisionChanged) PreviewState.SelectedMipIndex = 0;

		const uint32 MipCount = (Platform && Platform->IsValid())
			? static_cast<uint32>(Platform->Mips.size())
			: (Source ? 1u : 0u);

		if (MipCount == 0)
		{
			Preview.Release();
			PreviewState.LastUploadedMipIndex = UINT32_MAX;
			PreviewState.LastObservedRevision = Texture->GetBuildRevision();
			ImGui::TextWrapped("No preview data available.");
			return;
		}

		// Clamp selected mip to valid range.
		if (PreviewState.SelectedMipIndex >= MipCount) PreviewState.SelectedMipIndex = MipCount - 1;

		// Mip selector.
		if (MipCount > 1)
		{
			int MipInt = static_cast<int>(PreviewState.SelectedMipIndex);
			ImGui::SetNextItemWidth(MonaImGui::ScaleUI(200.0f));
			if (ImGui::SliderInt("Mip Level", &MipInt, 0, static_cast<int>(MipCount - 1), "%d", ImGuiSliderFlags_AlwaysClamp))
				PreviewState.SelectedMipIndex = static_cast<uint32>(MipInt);
			ImGui::SameLine();
			const uint32 MipWidth = Platform ? Platform->Mips[PreviewState.SelectedMipIndex].Width : Source->Width;
			const uint32 MipHeight = Platform ? Platform->Mips[PreviewState.SelectedMipIndex].Height : Source->Height;
			ImGui::TextDisabled("%s", FormatDimensions(MipWidth, MipHeight).c_str());
		}

		// Upload preview image.
		const bool bMipChanged = PreviewState.SelectedMipIndex != PreviewState.LastUploadedMipIndex;
		const bool bRebuildNeeded = bRevisionChanged || bMipChanged || !Preview.IsValid();
		if (bRebuildNeeded)
		{
			if (Platform && Platform->IsValid())
				Preview.Upload(*Platform, PreviewState.SelectedMipIndex);
			else if (Source && Source->IsValid())
				Preview.UploadSource(*Source);
			PreviewState.LastUploadedMipIndex = PreviewState.SelectedMipIndex;
			PreviewState.LastObservedRevision = Texture->GetBuildRevision();
		}

		// Draw the image.
		if (Preview.IsValid())
		{
			const ImVec2 Available = ImGui::GetContentRegionAvail();
			const float AspectRatio = static_cast<float>(Preview.GetWidth()) / static_cast<float>(std::max(Preview.GetHeight(), 1u));
			const float MaxPreviewHeight = MonaImGui::ScaleUI(512.0f);
			const float ImageHeight = std::max(0.0f, std::min(Available.y * 0.5f, MaxPreviewHeight));
			const float ImageWidth = ImageHeight * AspectRatio;

			if (ImageHeight > 0.0f && Mona::GActiveUIBackend)
			{
				ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - ImageWidth) * 0.5f + ImGui::GetCursorPosX());
				Mona::GActiveUIBackend->DrawImage(Preview.GetTexture(), FVector2f(ImageWidth, ImageHeight));
			}
		}
		else
		{
			ImGui::TextDisabled("Preview unavailable.");
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
		else
		{
			const ETextureBuildStatus Status = Texture->GetBuildStatus();
			if (Status == ETextureBuildStatus::DecodeFailure)
				DrawInfoRow("Status", "Source data unavailable (Decode failure)");
			else if (Status == ETextureBuildStatus::MissingSource)
				DrawInfoRow("Status", "Source data unavailable (Source file missing)");
			else
				DrawInfoRow("Status", "Source data unavailable");
		}
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
			DrawInfoRow("Status", GetEnumValueDisplayName("Durin::ETextureBuildStatus", static_cast<uint64>(Texture->GetBuildStatus())));
			DrawInfoRow("Pixel Format", GetPixelFormatInfo(Platform->PixelFormat).Name);
			DrawInfoRow("Mip Count", std::format("{}", Platform->Mips.size()));
			DrawInfoRow("Mip Range", std::format("{} to {}", FormatDimensions(Platform->Mips.front().Width, Platform->Mips.front().Height),
				FormatDimensions(Platform->Mips.back().Width, Platform->Mips.back().Height)));
			DrawInfoRow("Platform Bytes", FormatByteCount(TotalBytes));
			DrawInfoRow("Residency", "Fully resident");
		}
		else
		{
			DrawInfoRow("Status", GetEnumValueDisplayName("Durin::ETextureBuildStatus", static_cast<uint64>(Texture->GetBuildStatus())));
		}

		DrawInfoRow("Build Revision", std::format("{}", Texture->GetBuildRevision()));

		if (const std::shared_ptr<FTexture2DRenderResource>& Resource = Texture->GetRenderResource())
		{
			DrawInfoRow("GPU State", GetEnumValueDisplayName(
				"Durin::ERenderResourceState", static_cast<uint64>(Resource->GetResourceState())));
		}

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
