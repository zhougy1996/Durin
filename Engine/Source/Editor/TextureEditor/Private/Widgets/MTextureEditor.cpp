#include "Widgets/MTextureEditor.h"

#include "AssetSystem.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorWorkspaceUI.h"
#include "MonaImGui.h"
#include "MonaImGuiPropertyTable.h"
#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "PixelFormat.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DRenderResource.h"
#include "Widgets/TexturePreview.h"
#include "Workspace/TextureEditorWorkspace.h"

#include <cmath>

namespace Durin
{
	namespace
	{
		constexpr float DefaultPreviewPaneRatio = 0.70f;
		constexpr float WideLayoutMinimumWidth = 820.0f;
		constexpr float MinimumPreviewWidth = 440.0f;
		constexpr float MinimumDetailsWidth = 340.0f;
		constexpr float NarrowPreviewHeight = 430.0f;

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
				if (const FEnumValue* Record = Enum->FindValueRecordByValue(Value)) return Record->DisplayName;
			}
			return std::format("Unknown ({})", Value);
		}

		auto DrawTransparencyGrid(ImDrawList& DrawList, const ImVec2& Min, const ImVec2& Size) -> void
		{
			const float CheckerSize = MonaImGui::ScaleUI(12.0f);
			const ImVec2 Max(Min.x + Size.x, Min.y + Size.y);
			DrawList.PushClipRect(Min, Max, true);
			for (float Y = 0.0f; Y < Size.y; Y += CheckerSize)
				for (float X = 0.0f; X < Size.x; X += CheckerSize)
				{
					const bool bLight =
						(static_cast<int32>(X / CheckerSize) + static_cast<int32>(Y / CheckerSize)) % 2 == 0;
					DrawList.AddRectFilled(
						ImVec2(Min.x + X, Min.y + Y),
						ImVec2(Min.x + std::min(X + CheckerSize, Size.x), Min.y + std::min(Y + CheckerSize, Size.y)),
						ImGui::GetColorU32(bLight
							? ImVec4(0.30f, 0.30f, 0.30f, 1.0f)
							: ImVec4(0.20f, 0.20f, 0.20f, 1.0f)));
				}
			DrawList.PopClipRect();
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

	auto MTextureEditor::OpenDocument(const FEditorDocumentTab& Document) -> EEditorDocumentOpenResult
	{
		if (Document.ResourceId.empty()) return EEditorDocumentOpenResult::Rejected;
		if (FindOpenTexture(Document.ResourceId)) return EEditorDocumentOpenResult::Opened;
		FAssetPath AssetPath;
		std::string PathError;
		if (!FAssetPath::TryCreate(Document.ResourceId, AssetPath, &PathError))
		{
			SetError(std::move(PathError));
			return EEditorDocumentOpenResult::Rejected;
		}
		DTexture2D* Texture = nullptr;
		const Asset::FAssetResult Result = Asset::LoadAsset(AssetPath, Texture);
		if (!Result || !Texture)
		{
			SetError(Result ? "The selected asset is not a Texture2D." : Result.Message);
			return EEditorDocumentOpenResult::Rejected;
		}
		OpenTextures.emplace(Document.ResourceId, Texture);
		PreviewStates.try_emplace(Document.ResourceId);
		return EEditorDocumentOpenResult::Opened;
	}

	auto MTextureEditor::ActivateDocument(const FEditorDocumentTab& Document) -> void
	{
		DTexture2D* Texture = FindOpenTexture(Document.ResourceId);
		if (PropertyView.IsEditing() && !PropertyView.IsEditingObject(Texture) && !FinishActivePropertyEdit(true)) return;
		if (Texture) ActiveResourceId = Document.ResourceId;
		DocumentHost.RequestFocus(Document.Id);
	}

	auto MTextureEditor::RequestDeactivate() -> bool
	{
		return FinishActivePropertyEdit(true);
	}

	auto MTextureEditor::RequestCloseDocument(const FEditorDocumentTab& Document) -> EEditorDocumentCloseResult
	{
		if (PropertyView.IsEditingObject(FindOpenTexture(Document.ResourceId)) && !FinishActivePropertyEdit(true))
			return EEditorDocumentCloseResult::Rejected;
		if (IsDocumentDirty(Document)) return EEditorDocumentCloseResult::PendingConfirmation;
		OpenTextures.erase(Document.ResourceId);
		PreviewStates.erase(Document.ResourceId);
		if (ActiveResourceId == Document.ResourceId) ActiveResourceId.clear();
		return EEditorDocumentCloseResult::Closed;
	}

	auto MTextureEditor::SaveDocument(const FEditorDocumentTab& Document) -> bool
	{
		return SaveTexture(FindOpenTexture(Document.ResourceId));
	}

	auto MTextureEditor::DiscardDocument(const FEditorDocumentTab& Document) -> bool
	{
		DTexture2D* Texture = FindOpenTexture(Document.ResourceId);
		if (!Texture || !Texture->GetPackage()) return false;
		Texture->GetPackage()->ClearDirty();
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
		return DocumentHost.DrawDocuments(
			WorkspaceManager,
			TextureEditorWorkspace::Type,
			TextureEditorWorkspace::RootKey,
			[this](const FEditorDocumentTab& Document) {
				return FindOpenTexture(Document.ResourceId) != nullptr;
			},
			[this](const FEditorDocumentTab& Document) {
				DrawDocument(Document, FindOpenTexture(Document.ResourceId));
			}
		);
	}

	auto MTextureEditor::ResetLayout() -> void
	{
		PreviewPaneRatio = DefaultPreviewPaneRatio;
		for (auto& [ResourceId, State] : PreviewStates)
		{
			State.SelectedMipIndex = 0;
			State.Zoom = 0.0f;
			State.bShowCheckerboard = true;
			State.bPreviewSource = false;
		}
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

		DrawToolbar(Document, Texture);
		ImGui::Spacing();

		if (ImGui::GetContentRegionAvail().x >= MonaImGui::ScaleUI(WideLayoutMinimumWidth))
			DrawWideLayout(Document.ResourceId, Texture);
		else
			DrawNarrowLayout(Document.ResourceId, Texture);

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

	auto MTextureEditor::DrawToolbar(const FEditorDocumentTab& Document, DTexture2D* Texture) -> void
	{
		if (ImGui::Button("Save")) SaveTexture(Texture);
		ImGui::SameLine();
		if (ImGui::Button("Refresh"))
		{
			std::string Error;
			if (!Texture->PostLoad(Error)) SetError(std::move(Error));
		}
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		ImGui::TextUnformatted(Document.ResourceId.c_str());

		const ETextureBuildStatus Status = Texture->GetBuildStatus();
		const std::string StatusName =
			GetEnumValueDisplayName("Durin::ETextureBuildStatus", static_cast<uint64>(Status));
		const ImVec4 StatusColor = Status == ETextureBuildStatus::Ready
			? ImVec4(0.40f, 0.85f, 0.52f, 1.0f)
			: (Status == ETextureBuildStatus::Unbuilt
				? ImVec4(0.75f, 0.75f, 0.75f, 1.0f)
				: ImVec4(1.0f, 0.48f, 0.35f, 1.0f));
		const float StatusWidth = ImGui::CalcTextSize(StatusName.c_str()).x;
		const float RightX = ImGui::GetWindowContentRegionMax().x - StatusWidth;
		if (ImGui::GetCursorPosX() < RightX)
		{
			ImGui::SameLine();
			ImGui::SetCursorPosX(RightX);
			ImGui::TextColored(StatusColor, "%s", StatusName.c_str());
		}
	}

	auto MTextureEditor::DrawWideLayout(const std::string& ResourceId, DTexture2D* Texture) -> void
	{
		const MonaImGui::FUIStyleMetrics Metrics = MonaImGui::GetUIStyleMetrics();
		const ImVec2 Available = ImGui::GetContentRegionAvail();
		const float MinimumPreview = MonaImGui::ScaleUI(MinimumPreviewWidth);
		const float MinimumDetails = MonaImGui::ScaleUI(MinimumDetailsWidth);
		const float PreviewWidth = std::clamp(
			Available.x * PreviewPaneRatio,
			MinimumPreview,
			std::max(MinimumPreview, Available.x - Metrics.SplitterThickness - MinimumDetails));

		DrawPreviewPanel(ResourceId, Texture, PreviewWidth, Available.y);
		ImGui::SameLine();
		MonaImGui::DrawSplitter(
			"TextureEditorDetailsSplitter",
			MonaImGui::EUISplitterAxis::X,
			Available.y,
			Available.x,
			MinimumPreview,
			MinimumDetails,
			PreviewPaneRatio);
		ImGui::SameLine();
		DrawDetailsPanel(Texture, Available.y);
	}

	auto MTextureEditor::DrawNarrowLayout(const std::string& ResourceId, DTexture2D* Texture) -> void
	{
		DrawPreviewPanel(ResourceId, Texture, 0.0f, MonaImGui::ScaleUI(NarrowPreviewHeight));
		ImGui::Spacing();
		DrawDetailsPanel(Texture, 0.0f);
	}

	auto MTextureEditor::DrawDetailsPanel(DTexture2D* Texture, float Height) -> void
	{
		if (ImGui::BeginChild("TextureDetails", ImVec2(0.0f, Height), ImGuiChildFlags_Borders))
		{
			ImGui::TextDisabled("TEXTURE DETAILS");
			ImGui::Separator();
			DrawFailureState(Texture);
			if (ImGui::CollapsingHeader("Build Settings", ImGuiTreeNodeFlags_DefaultOpen))
				DrawBuildSettings(Texture);
			if (ImGui::CollapsingHeader("Source Information", ImGuiTreeNodeFlags_DefaultOpen))
				DrawSourceData(Texture);
		}
		ImGui::EndChild();
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

	auto MTextureEditor::DrawPreviewPanel(
		const std::string& ResourceId,
		DTexture2D* Texture,
		float Width,
		float Height
	) -> void
	{
		FTexturePreviewState& PreviewState = PreviewStates.try_emplace(ResourceId).first->second;
		FTexturePreview& Preview = *PreviewState.Preview;

		const FTexturePlatformData* Platform = Texture->GetPlatformData();
		const FTextureSourceData* Source = Texture->GetSourceData();
		const bool bSourceAvailable = Source && Source->IsValid();
		const bool bPlatformAvailable = Platform && Platform->IsValid();
		if (PreviewState.bPreviewSource && !bSourceAvailable) PreviewState.bPreviewSource = false;
		if (!bPlatformAvailable && bSourceAvailable) PreviewState.bPreviewSource = true;

		const bool bRevisionChanged = Texture->GetBuildRevision() != PreviewState.LastObservedRevision;
		if (bRevisionChanged) PreviewState.SelectedMipIndex = 0;

		const uint32 MipCount = (!PreviewState.bPreviewSource && bPlatformAvailable)
			? static_cast<uint32>(Platform->Mips.size())
			: (bSourceAvailable ? 1u : 0u);

		if (ImGui::BeginChild("TexturePreviewPanel", ImVec2(Width, Height), ImGuiChildFlags_Borders))
		{
			ImGui::TextDisabled("PREVIEW");
			ImGui::SameLine();
			if (!bPlatformAvailable) ImGui::BeginDisabled();
			if (ImGui::RadioButton("Built", !PreviewState.bPreviewSource))
				PreviewState.bPreviewSource = false;
			if (!bPlatformAvailable) ImGui::EndDisabled();
			ImGui::SameLine();
			if (!bSourceAvailable) ImGui::BeginDisabled();
			if (ImGui::RadioButton("Source", PreviewState.bPreviewSource))
				PreviewState.bPreviewSource = true;
			if (!bSourceAvailable) ImGui::EndDisabled();
			if (!bSourceAvailable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("Decoded source pixels are not resident.");

			ImGui::SameLine();
			ImGui::TextDisabled("|");
			ImGui::SameLine();
			if (ImGui::Button("Fit")) PreviewState.Zoom = 0.0f;
			ImGui::SameLine();
			if (ImGui::Button("1:1")) PreviewState.Zoom = 1.0f;
			ImGui::SameLine();
			ImGui::Checkbox("Checkerboard", &PreviewState.bShowCheckerboard);

			if (MipCount == 0)
			{
				Preview.Release();
				PreviewState.LastUploadedMipIndex = UINT32_MAX;
				PreviewState.LastObservedRevision = Texture->GetBuildRevision();
				ImGui::Separator();
				ImGui::TextDisabled("No preview data is available.");
				ImGui::EndChild();
				return;
			}

			if (PreviewState.SelectedMipIndex >= MipCount) PreviewState.SelectedMipIndex = MipCount - 1;
			if (MipCount > 1)
			{
				ImGui::SameLine();
				ImGui::SetNextItemWidth(MonaImGui::ScaleUI(150.0f));
				int MipIndex = static_cast<int>(PreviewState.SelectedMipIndex);
				if (ImGui::SliderInt("Mip", &MipIndex, 0, static_cast<int>(MipCount - 1), "%d", ImGuiSliderFlags_AlwaysClamp))
					PreviewState.SelectedMipIndex = static_cast<uint32>(MipIndex);
			}

			const bool bMipChanged = PreviewState.SelectedMipIndex != PreviewState.LastUploadedMipIndex;
			const bool bPreviewModeChanged = PreviewState.bPreviewSource != PreviewState.bLastUploadWasSource;
			if (bRevisionChanged || bMipChanged || bPreviewModeChanged || !Preview.IsValid())
			{
				if (PreviewState.bPreviewSource)
					Preview.UploadSource(*Source);
				else
					Preview.Upload(*Platform, PreviewState.SelectedMipIndex);
				PreviewState.LastUploadedMipIndex = PreviewState.SelectedMipIndex;
				PreviewState.LastObservedRevision = Texture->GetBuildRevision();
				PreviewState.bLastUploadWasSource = PreviewState.bPreviewSource;
			}

			ImGui::Separator();
			if (ImGui::BeginChild(
				"TexturePreviewCanvas",
				ImVec2(0.0f, 0.0f),
				ImGuiChildFlags_None,
				ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
			{
				const ImVec2 CanvasSize = ImGui::GetContentRegionAvail();
				const float Padding = MonaImGui::ScaleUI(24.0f);
				const float FitScale = std::clamp(
					std::min(
						(CanvasSize.x - Padding * 2.0f) / static_cast<float>(Preview.GetWidth()),
						(CanvasSize.y - Padding * 2.0f) / static_cast<float>(Preview.GetHeight())),
					0.01f,
					16.0f);
				if (ImGui::IsWindowHovered() && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0.0f)
				{
					const float CurrentZoom = PreviewState.Zoom > 0.0f ? PreviewState.Zoom : FitScale;
					PreviewState.Zoom = std::clamp(
						CurrentZoom * std::pow(1.25f, ImGui::GetIO().MouseWheel),
						0.05f,
						16.0f);
				}
				if (ImGui::IsWindowHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
				{
					ImGui::SetScrollX(ImGui::GetScrollX() - ImGui::GetIO().MouseDelta.x);
					ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);
				}

				const float Scale = PreviewState.Zoom > 0.0f ? PreviewState.Zoom : FitScale;
				const ImVec2 ImageSize(
					std::max(static_cast<float>(Preview.GetWidth()) * Scale, 1.0f),
					std::max(static_cast<float>(Preview.GetHeight()) * Scale, 1.0f));
				const ImVec2 Start = ImGui::GetCursorPos();
				ImGui::SetCursorPos(ImVec2(
					Start.x + std::max((CanvasSize.x - ImageSize.x) * 0.5f, Padding),
					Start.y + std::max((CanvasSize.y - ImageSize.y) * 0.5f, Padding)));
				const ImVec2 ImageMin = ImGui::GetCursorScreenPos();
				if (PreviewState.bShowCheckerboard)
					DrawTransparencyGrid(*ImGui::GetWindowDrawList(), ImageMin, ImageSize);
				if (Mona::GActiveUIBackend)
					Mona::GActiveUIBackend->DrawImage(Preview.GetTexture(), FVector2f(ImageSize.x, ImageSize.y));

				const std::string Overlay = std::format(
					"{}  |  {}%",
					FormatDimensions(Preview.GetWidth(), Preview.GetHeight()),
					static_cast<int>(std::round(Scale * 100.0f)));
				const ImVec2 OverlaySize = ImGui::CalcTextSize(Overlay.c_str());
				const ImVec2 OverlayMin(
					ImGui::GetWindowPos().x + Padding,
					ImGui::GetWindowPos().y + ImGui::GetWindowSize().y - OverlaySize.y - Padding);
				ImGui::GetWindowDrawList()->AddRectFilled(
					ImVec2(OverlayMin.x - 7.0f, OverlayMin.y - 4.0f),
					ImVec2(OverlayMin.x + OverlaySize.x + 7.0f, OverlayMin.y + OverlaySize.y + 4.0f),
					IM_COL32(12, 14, 18, 205),
					4.0f);
				ImGui::GetWindowDrawList()->AddText(OverlayMin, ImGui::GetColorU32(ImGuiCol_Text), Overlay.c_str());
				if (ImGui::IsWindowHovered())
					ImGui::SetTooltip("Ctrl + mouse wheel to zoom\nMiddle-drag to pan");
			}
			ImGui::EndChild();
		}
		ImGui::EndChild();
	}

	auto MTextureEditor::DrawSourceData(DTexture2D* Texture) -> void
	{
		if (!MonaImGui::PropertyEdit::BeginTable("TextureSourceData")) return;
		DrawInfoRow("Source Path", Texture->GetSourceFile());
		const FTextureSourceDiagnostic SourceDiagnostic = Texture->InspectSource();
		switch (SourceDiagnostic.Status)
		{
		case ETextureSourceStatus::Available:
			DrawInfoRow("Provenance", "Portable source available");
			break;
		case ETextureSourceStatus::Missing:
			DrawInfoRow("Provenance", "Portable source missing");
			break;
		case ETextureSourceStatus::Invalid:
			DrawInfoRow("Provenance", "Invalid or unsupported source metadata");
			break;
		case ETextureSourceStatus::NoSource:
			DrawInfoRow("Provenance", "No source dependency");
			break;
		}
		if (const FTextureSourceData* Source = Texture->GetSourceData())
		{
			DrawInfoRow("Dimensions", FormatDimensions(Source->Width, Source->Height));
			DrawInfoRow("Source Channels", std::format("{}", Source->SourceChannelCount));
			DrawInfoRow("Transparency", Source->bHasTransparency ? "Present" : "Opaque");
			DrawInfoRow("Decoded Format", Source->Format == ETextureSourceFormat::RGBA8 ? "RGBA8" : "Invalid");
		}
		else if (Texture->GetSourceWidth() > 0 && Texture->GetSourceHeight() > 0)
		{
			DrawInfoRow("Dimensions", FormatDimensions(Texture->GetSourceWidth(), Texture->GetSourceHeight()));
			DrawInfoRow("Source Channels", std::format("{}", Texture->GetSourceChannelCount()));
			DrawInfoRow("Transparency", Texture->SourceHasTransparency() ? "Present" : "Opaque");
			DrawInfoRow("Decoded Format", "Not resident (derived-data cache hit)");
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
		if (!MonaImGui::PropertyEdit::BeginTable("TextureBuildSettings")) return;
		FProperty* UsageProperty = Texture->GetClass()->FindPropertyByName("Usage");
		FProperty* SRGBProperty = Texture->GetClass()->FindPropertyByName("bSRGB");
		FProperty* MaxResolutionProperty = Texture->GetClass()->FindPropertyByName("MaxResolution");
		FProperty* CompressionQualityProperty = Texture->GetClass()->FindPropertyByName("CompressionQuality");
		FProperty* AlphaMipModeProperty = Texture->GetClass()->FindPropertyByName("AlphaMipMode");
		FProperty* AlphaCoverageThresholdProperty = Texture->GetClass()->FindPropertyByName("AlphaCoverageThreshold");
		if (UsageProperty) PropertyView.EditProperty(MakePropertyViewContext(), Texture, UsageProperty, 0, {.Label = "Usage"});
		else DrawInfoRow("Usage", "Reflection metadata unavailable");
		if (SRGBProperty) PropertyView.EditProperty(MakePropertyViewContext(), Texture, SRGBProperty, 0, {.Label = "sRGB"});
		else DrawInfoRow("sRGB", "Reflection metadata unavailable");
		if (MaxResolutionProperty) PropertyView.EditProperty(MakePropertyViewContext(), Texture, MaxResolutionProperty, 0,
			{.Label = "Max Resolution (0 = source)"});
		else DrawInfoRow("Max Resolution", "Reflection metadata unavailable");
		if (CompressionQualityProperty) PropertyView.EditProperty(MakePropertyViewContext(), Texture, CompressionQualityProperty, 0,
			{.Label = "Compression Quality"});
		else DrawInfoRow("Compression Quality", "Reflection metadata unavailable");
		if (AlphaMipModeProperty) PropertyView.EditProperty(MakePropertyViewContext(), Texture, AlphaMipModeProperty, 0,
			{.Label = "Alpha Mip Mode"});
		else DrawInfoRow("Alpha Mip Mode", "Reflection metadata unavailable");
		if (AlphaCoverageThresholdProperty) PropertyView.EditProperty(MakePropertyViewContext(), Texture,
			AlphaCoverageThresholdProperty, 0, {.Label = "Alpha Coverage Threshold"});
		else DrawInfoRow("Alpha Coverage Threshold", "Reflection metadata unavailable");

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
