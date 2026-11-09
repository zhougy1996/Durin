#include "Widgets/MTextureEditor.h"
#include "RenderResource.h"
#include "PropertyEditor/DetailPanel.h"

#include "Asset/AssetCompilingManager.h"
#include "Asset/Asset.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "Dialogs/FileDialog.h"
#include "Diagnostics/TexturePayloadInspection.h"
#include "Editor/EditorEngine.h"
#include "Documents/EditorDocumentManager.h"
#include "Toolkits/AssetEditor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Framework/TabLayout.h"
#include "MonaImGui.h"
#include "Widgets/MonaImGuiPropertyTable.h"
#include "Widgets/MonaImGuiWidgets.h"
#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "Misc/StringHelper.h"
#include "PixelFormat.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DCompilation.h"
#include "Texture/Texture2DRenderResource.h"
#include "Import/EditorReimportHandler.h"
#include "TexturePreview.h"
#include "Workspace/TextureEditorWorkspace.h"

namespace Durin::Editor::Texture
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

		auto PayloadStageName(ETexturePayloadStage Stage) -> const char*
		{
			switch (Stage)
			{
			case ETexturePayloadStage::Source: return "Source";
			case ETexturePayloadStage::DerivedData: return "Derived";
			case ETexturePayloadStage::Cooked: return "Cooked";
			case ETexturePayloadStage::Decoded: return "Decoded";
			case ETexturePayloadStage::RuntimeResource: return "GPU";
			}
			return "Unknown";
		}

		auto PayloadStateName(ETexturePayloadState State) -> const char*
		{
			switch (State)
			{
			case ETexturePayloadState::Unknown: return "Unknown";
			case ETexturePayloadState::NotPresent: return "Not present";
			case ETexturePayloadState::Available: return "Available";
			case ETexturePayloadState::Missing: return "Missing";
			case ETexturePayloadState::Stale: return "Stale";
			case ETexturePayloadState::Corrupt: return "Corrupt";
			case ETexturePayloadState::Unsupported: return "Unsupported";
			case ETexturePayloadState::Failed: return "Failed";
			}
			return "Unknown";
		}

		auto PayloadRepairName(ETexturePayloadRepairAction Repair) -> const char*
		{
			switch (Repair)
			{
			case ETexturePayloadRepairAction::None: return "None";
			case ETexturePayloadRepairAction::ReimportSource: return "Reimport source";
			case ETexturePayloadRepairAction::RebuildDerivedData: return "Rebuild derived data";
			case ETexturePayloadRepairAction::RestoreEditorCompanion: return "Restore editor companion";
			case ETexturePayloadRepairAction::Recook: return "Recook";
			case ETexturePayloadRepairAction::RemoveOrphan: return "Remove orphan";
			case ETexturePayloadRepairAction::UpgradeOrResave: return "Upgrade or resave";
			}
			return "None";
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

	MTextureEditor::MTextureEditor(FEditorDocumentManager& InWorkspaceManager)
		: DocumentManager(InWorkspaceManager)
	{
		if (auto* Service = DocumentManager.GetAssetEditorSubsystem()) Documents.BindEditor(Service->GetEditor());

	}

	MTextureEditor::~MTextureEditor()
	{

		FinishActivePropertyEdit(true);
		for (auto& [ResourceId, Texture] : OpenTextures)
		{
			(void)ResourceId;
			CancelUnobservedCompilation(Texture.Get());
		}
	}

	auto MTextureEditor::GetToolkitId() const -> const FAssetEditorToolkitId&
	{
		return Workspace::Type;
	}

	auto MTextureEditor::OpenDocument(const FEditorDocument& Document) -> EDocumentOpenResult
	{
		if (Document.ResourceId.empty()) return EDocumentOpenResult::Rejected;
		if (FindOpenTexture(Document.ResourceId)) return EDocumentOpenResult::Opened;
		FObjectPath AssetPath;
		std::string PathError;
		if (const auto PathValidation = FObjectPath::TryCreateWithDiagnostic(Document.ResourceId, AssetPath); !PathValidation)
		{
			PathError = ToString(PathValidation.error());
			SetError(std::move(PathError));
			return EDocumentOpenResult::Rejected;
		}
		DTexture2D* Texture = nullptr;
		const auto Result = LoadObject<DTexture2D>(AssetPath);
		Texture = Result.value_or(nullptr);
		if (!Result || !Texture)
		{
			SetError(Result ? "The selected asset is not a Texture2D." : (Result ? std::string{} : Result.error().Message));
			return EDocumentOpenResult::Rejected;
		}
		OpenTextures.emplace(Document.ResourceId, Texture);
		PreviewStates.try_emplace(Document.ResourceId);
		return EDocumentOpenResult::Opened;
	}

	auto MTextureEditor::ActivateDocument(const FEditorDocument& Document) -> void
	{
		DTexture2D* Texture = FindOpenTexture(Document.ResourceId);
		if (PropertyView.IsEditing() && !PropertyView.IsEditingObject(Texture) && !FinishActivePropertyEdit(true)) return;
		if (IsReadOnly()) Documents.ActivateReadOnly(Document, Texture);
		else Documents.Activate(Document, Texture);
	}

	auto MTextureEditor::RequestDeactivate() -> bool
	{
		return FinishActivePropertyEdit(true);
	}

	auto MTextureEditor::RequestCloseDocument(const FEditorDocument& Document) -> EDocumentCloseResult
	{
		if (PropertyView.IsEditingObject(FindOpenTexture(Document.ResourceId)) && !FinishActivePropertyEdit(true))
			return EDocumentCloseResult::Rejected;
		if (IsDocumentDirty(Document)) return EDocumentCloseResult::PendingConfirmation;
		CancelUnobservedCompilation(FindOpenTexture(Document.ResourceId));
		OpenTextures.erase(Document.ResourceId);
		PreviewStates.erase(Document.ResourceId);
		Documents.Close(Document.ResourceId);
		return EDocumentCloseResult::Closed;
	}

	auto MTextureEditor::OnSaveDocument(const FEditorDocument& Document) -> bool
	{
		return SaveTexture(FindOpenTexture(Document.ResourceId));
	}

	auto MTextureEditor::OnDiscardDocument(const FEditorDocument& Document) -> bool
	{
		DTexture2D* Texture = FindOpenTexture(Document.ResourceId);
		return Documents.Discard(Texture, [Texture] {
			FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Texture);
		}, [this](DPackage* Previous, DPackage* Replacement) {
			DocumentManager.NotifyPackageReloaded(Previous, Replacement);
		}, [this](std::string Message) { SetError(std::move(Message)); }, &DocumentManager);
	}

	auto MTextureEditor::OnPackageReloaded(DPackage* Previous, DPackage* Replacement) -> void
	{
		for (auto& [ResourceId, Texture] : OpenTextures)
			if (Texture.Get() && Texture->GetPackage() == Previous)
			{
				Texture = Cast<DTexture2D>(Replacement->FindTopLevelAsset(Texture->GetFName()));
				if (auto It = PreviewStates.find(ResourceId); It != PreviewStates.end())
				{
					It->second.Preview = std::make_unique<FTexturePreview>();
					It->second.PlatformInput.reset();
					It->second.LastUploadedMipIndex = UINT32_MAX;
				}
			}
	}

	auto MTextureEditor::OnResourceIdRemapped(std::string_view Source, std::string_view Destination) -> void
	{
		if (auto Entry = OpenTextures.extract(std::string(Source)); !Entry.empty())
		{
			Entry.key() = Destination;
			OpenTextures.insert(std::move(Entry));
		}
		if (auto Entry = PreviewStates.extract(std::string(Source)); !Entry.empty())
		{
			Entry.key() = Destination;
			PreviewStates.insert(std::move(Entry));
		}
		Documents.RemapResourceId(Source, Destination);
	}

	auto MTextureEditor::OnIsDocumentDirty(const FEditorDocument& Document) const -> bool
	{
		return Documents.IsDirty(FindOpenTexture(Document.ResourceId));
	}

	auto MTextureEditor::OnCanSaveActiveDocument() const -> bool
	{
		DTexture2D* Texture = GetActiveTexture();
		return Texture && Texture->IsAsyncCacheComplete()
			&& Documents.CanSave(Texture);
	}

	auto MTextureEditor::OnSaveActiveDocument() -> bool
	{
		return SaveTexture(GetActiveTexture());
	}

	auto MTextureEditor::OnCanUndo() const -> bool
	{
		return Documents.CanUndo();
	}

	auto MTextureEditor::OnCanRedo() const -> bool
	{
		return Documents.CanRedo();
	}

	auto MTextureEditor::GetUndoDescription() const -> std::string
	{
		return Documents.GetUndoDescription();
	}

	auto MTextureEditor::GetRedoDescription() const -> std::string
	{
		return Documents.GetRedoDescription();
	}

	auto MTextureEditor::OnUndo() -> bool
	{
		return FinishActivePropertyEdit(false) && Documents.Undo();
	}

	auto MTextureEditor::OnRedo() -> bool
	{
		return FinishActivePropertyEdit(false) && Documents.Redo();
	}

	auto MTextureEditor::OnDrawToolkit(bool bActive) -> bool
	{
		if (!bActive && PropertyView.IsEditing()) FinishActivePropertyEdit(true);
		const auto RootKey = GetPresentationRootKey(Workspace::RootKey);
		return Documents.GetTabManager().DrawDocuments(
			DocumentManager,
			Workspace::Type,
			RootKey,
			[this](const FEditorDocument& Document) {
				return FindOpenTexture(Document.ResourceId) != nullptr;
			},
			[this](const FEditorDocument& Document) {
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
		return FindOpenTexture(Documents.GetActiveResourceId());
	}

	auto MTextureEditor::SaveTexture(DTexture2D* Texture) -> bool
	{
		if (IsReadOnly()) return false;
		return Documents.Save(Texture, [this, Texture] {
			if (Texture->IsAsyncCacheComplete()) return true;
			SetError(
				"This texture is still building. Save when the build finishes, "
				"or cancel it to keep the last successful state.");
			return false;
		}, [this](std::string Message) { SetError(std::move(Message)); });
	}

	auto MTextureEditor::CancelUnobservedCompilation(DTexture2D* Texture) -> void
	{
		if (!Texture || IsReadOnly()) return;
		auto* Owner = GetOwningAssetEditor();
		auto* Service = Owner ? Owner->GetAssetEditorSubsystem() : DocumentManager.GetAssetEditorSubsystem();
		if (Service)
		{
			const auto Observers = Service->FindEditorsForAsset(Texture);
			if (!Owner && Observers.size() > 1) return;
			if (Owner)
				for (auto* Instance : Observers) if (Instance != Owner) return;
		}
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Texture);
	}

	auto MTextureEditor::DrawDocument(const FEditorDocument& Document, DTexture2D* Texture) -> void
	{

		DrawToolbar(Document, Texture);
		ImGui::Spacing();

		if (ImGui::GetContentRegionAvail().x >= MonaImGui::ScaleUI(WideLayoutMinimumWidth))
			DrawWideLayout(Document.ResourceId, Texture);
		else
			DrawNarrowLayout(Document.ResourceId, Texture);

		if (Documents.GetActiveResourceId() != Document.ResourceId) return;
		MonaImGui::ErrorDialog("Texture Editor Error", ErrorMessage);
	}

	auto MTextureEditor::DrawToolbar(const FEditorDocument& Document, DTexture2D* Texture) -> void
	{
		const bool bPending = !Texture->IsAsyncCacheComplete();
		ImGui::BeginDisabled(IsReadOnly());
		if (bPending) ImGui::BeginDisabled();
		if (ImGui::Button("Save")) SaveTexture(Texture);
		if (bPending) ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Refresh"))
		{
			Texture->PostLoad();
		}
		if (bPending)
		{
			ImGui::SameLine();
			if (ImGui::Button("Cancel Build"))
				FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Texture);
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		ImGui::TextUnformatted(Document.ResourceId.c_str());

		const bool bReady = Texture->HasPlatformData();
		const std::string StatusName = bPending ? "Building" : bReady ? "CPU Ready" : "Not Built";
		const ImVec4 StatusColor = bReady ? ImVec4(0.40f, 0.85f, 0.52f, 1.0f)
			: ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
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
			if (ImGui::CollapsingHeader("Payload Lifecycle", ImGuiTreeNodeFlags_DefaultOpen))
				DrawPayloadLifecycle(Texture);
			if (ImGui::CollapsingHeader("Build Settings", ImGuiTreeNodeFlags_DefaultOpen))
				DrawBuildSettings(Texture);
			if (ImGui::CollapsingHeader("Source Information", ImGuiTreeNodeFlags_DefaultOpen))
				DrawSourceData(Texture);
		}
		ImGui::EndChild();
	}

	auto MTextureEditor::DrawPayloadLifecycle(DTexture2D* Texture) -> void
	{
		if (!Texture || !MonaImGui::PropertyEdit::BeginTable("TexturePayloadLifecycle"))
			return;
		const FTexturePayloadInspection Inspection = InspectTexturePayloads(*Texture);
		for (const FTexturePayloadInspectionEntry& Entry : Inspection.Entries)
		{
			DrawInfoRow(PayloadStageName(Entry.Stage), std::format(
				"{} | {} | {} | repair: {}",
				PayloadStateName(Entry.State), Entry.Placement,
				StringUtils::FormatByteSize(Entry.LogicalByteCount), PayloadRepairName(Entry.Repair)));
			if (!Entry.Diagnostic.empty()) DrawInfoRow("Diagnostic", Entry.Diagnostic);
		}
		MonaImGui::PropertyEdit::EndTable();
	}

	auto MTextureEditor::DrawFailureState(DTexture2D* Texture) -> void
	{
		const bool bPending = !Texture->IsAsyncCacheComplete();
		if (bPending || Texture->HasPlatformData() || !Texture->GetSource().IsValid()) return;
		const char* Title = "Platform Data Unavailable";
		ImVec4 TitleColor(1.0f, 0.5f, 0.3f, 1.0f); // Amber default
		const char* Message = "The platform texture data is not available. Rebuild the texture; see the log if it fails.";

		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.18f, 0.08f, 0.08f, 0.5f));
		ImGui::BeginChild("TextureFailureState", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
		ImGui::TextColored(TitleColor, "%s", Title);
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.9f, 0.85f, 1.0f));
		ImGui::TextWrapped("%s", Message);
		ImGui::PopStyleColor();

		ImGui::Spacing();
		ImGui::BeginDisabled(IsReadOnly());
		if (ImGui::Button("Retry Build"))
		{
			Texture->PostLoad();
		}
		ImGui::EndDisabled();

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
		const bool bSourceAvailable = Texture->GetSource().IsValid()
			&& Texture->GetSource().GetFormat() == ETextureSourceFormat::RGBA8;
		const bool bPlatformAvailable = Platform && Platform->IsValid();
		if (PreviewState.bPreviewSource && !bSourceAvailable) PreviewState.bPreviewSource = false;

		const bool bRevisionChanged = PreviewState.PlatformInput.lock() != Texture->GetPlatformDataShared() || PreviewState.SourceIdentity != Texture->GetSource().GetIdentity();
		if (bRevisionChanged) PreviewState.SelectedMipIndex = 0;

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
				ImGui::SetTooltip("RGBA8 source data is unavailable.");

			ImGui::SameLine();
			ImGui::TextDisabled("|");
			ImGui::SameLine();
			if (ImGui::Button("Fit")) PreviewState.Zoom = 0.0f;
			ImGui::SameLine();
			if (ImGui::Button("1:1")) PreviewState.Zoom = 1.0f;
			ImGui::SameLine();
			ImGui::Checkbox("Checkerboard", &PreviewState.bShowCheckerboard);

			ImGui::Spacing();
			ImGui::TextDisabled("Channel");
			constexpr std::array ChannelLabels = {"RGBA", "R", "G", "B", "A"};
			constexpr std::array ChannelValues = {
				ETexturePreviewChannel::RGBA,
				ETexturePreviewChannel::Red,
				ETexturePreviewChannel::Green,
				ETexturePreviewChannel::Blue,
				ETexturePreviewChannel::Alpha,
			};
			for (size_t ChannelIndex = 0; ChannelIndex < ChannelLabels.size(); ++ChannelIndex)
			{
				ImGui::SameLine();
				if (ImGui::RadioButton(
						ChannelLabels[ChannelIndex],
						PreviewState.SelectedChannel == ChannelValues[ChannelIndex]))
				{
					PreviewState.SelectedChannel = ChannelValues[ChannelIndex];
				}
			}

			ImGui::TextDisabled("Display");
			constexpr std::array ModeLabels = {"Auto", "Raw", "Normal"};
			for (uint32 Index = 0; Index < ModeLabels.size(); ++Index)
			{
				ImGui::SameLine();
				const auto Mode = static_cast<ETexturePreviewInterpretation>(Index);
				if (ImGui::RadioButton(ModeLabels[Index], PreviewState.Interpretation == Mode))
					PreviewState.Interpretation = Mode;
			}
			const FTexturePreviewOptions Options{
				.Usage = Texture->GetUsage(), .Interpretation = PreviewState.Interpretation,
				.Channel = PreviewState.SelectedChannel};

			const uint32 MipCount = PreviewState.bPreviewSource
				? (bSourceAvailable ? 1u : 0u)
				: (bPlatformAvailable ? static_cast<uint32>(Platform->Mips.size()) : 0u);
			if (MipCount == 0)
			{
				Preview.Release();
				PreviewState.LastUploadedMipIndex = UINT32_MAX;
				PreviewState.PlatformInput = Texture->GetPlatformDataShared();
				PreviewState.SourceIdentity = Texture->GetSource().GetIdentity();
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
			const bool bChannelChanged = Options != PreviewState.LastAppliedOptions;
			if (bRevisionChanged || bMipChanged || bPreviewModeChanged || !Preview.IsValid())
			{
				if (PreviewState.bPreviewSource)
				{
					const auto Mips = Texture->GetSource().GetMipData();
					const auto View = Mips.IsValid() ? Mips.GetMipImage(0, 0, 0) : Image::FImageView{};
					Preview.Release();
					Preview.UploadSource(View, Options);
				}
				else
					Preview.Upload(*Platform, PreviewState.SelectedMipIndex, Options);
				PreviewState.LastUploadedMipIndex = PreviewState.SelectedMipIndex;
				PreviewState.PlatformInput = Texture->GetPlatformDataShared();
				PreviewState.SourceIdentity = Texture->GetSource().GetIdentity();
				PreviewState.bLastUploadWasSource = PreviewState.bPreviewSource;
				PreviewState.LastAppliedOptions = Options;
			}
			else if (bChannelChanged)
			{
				Preview.SetOptions(Options);
				PreviewState.LastAppliedOptions = Options;
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
				bool bDrewPreview = false;
				ImGui::PushID("TexturePreviewImage");
				if (Mona::GetActiveUIBackend())
					bDrewPreview = Mona::GetActiveUIBackend()->DrawImage(
						Preview.GetTexture(), FVector2f(ImageSize.x, ImageSize.y));
				ImGui::PopID();
				if (!bDrewPreview) ImGui::Dummy(ImageSize);

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
		SourceReferenceIndex.RequestRefresh();
		const DAssetImportData* ImportData = Texture->GetAssetImportData();
		const FSourceFile* ImportedSource = ImportData
			? ImportData->GetSourceData().FindByRole("source") : nullptr;
		const std::string_view SourceHint = ImportedSource
			? std::string_view(ImportedSource->Hint) : std::string_view{};
		if (!MonaImGui::PropertyEdit::BeginTable("TextureSourceData")) return;
		DrawInfoRow("Asset Destination",
			Texture->GetPackage() ? Texture->GetPackage()->GetPackagePath() : "");
		DrawInfoRow("Reimport Hint", SourceHint.empty() ? "Not retained" : SourceHint);
		if (!SourceHint.empty())
		{
			if (SourceReferenceIndex.IsCurrent())
			{
				const std::span<const FSourceReference> References =
					SourceReferenceIndex.FindReferences(SourceHint);
				DrawInfoRow("Shared References", std::format("{} asset(s)", References.size()));
			}
			else
			{
				DrawInfoRow("Shared References", "Building source reference index...");
			}
		}
		const FTextureSource& Source = Texture->GetSource();
		if (Source.IsValid())
		{
			DrawInfoRow("Dimensions", FormatDimensions(Source.GetWidth(), Source.GetHeight()));
			DrawInfoRow("Source Channels", std::format("{}", Source.GetSourceChannelCount()));
			DrawInfoRow("Transparency", Source.HasTransparency() ? "Present" : "Opaque");
			DrawInfoRow("Decoded Format", Source.GetFormat() == ETextureSourceFormat::RGBA8 ? "RGBA8" : "Invalid");
		}
		else
		{
			DrawInfoRow("Status", "Source data unavailable");
		}
		MonaImGui::PropertyEdit::EndTable();
		const bool bCanReimport = !SourceHint.empty();
		ImGui::BeginDisabled(IsReadOnly());
		constexpr const char* ReimportLabel = "Reimport";
		if (!bCanReimport) ImGui::BeginDisabled();
		if (ImGui::Button(ReimportLabel))
			ReimportSource(Texture);
		if (!bCanReimport) ImGui::EndDisabled();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", bCanReimport
				? "Captures and reimports the retained optional source hint."
				: "Reimport is unavailable because no source hint is retained.");
		ImGui::SameLine();
		if (ImGui::Button("Reimport From File...")) ReimportFromFile(Texture);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Captures a selected file and commits it only after decode and Build succeed.");
		ImGui::EndDisabled();
	}

	auto MTextureEditor::ReimportSource(DTexture2D* Texture) -> void
	{
		if (!Texture || IsReadOnly()) return;
		FReimportManager::Reimport(*Texture, {}, [Weak = weak_from_this()](FReimportResult Result) {
			const auto Toolkit = Weak.lock();
			if (!Toolkit || !Toolkit->IsSessionActive()) return;
			auto* Self = static_cast<MTextureEditor*>(Toolkit.get());
			if (Result.Interrupted()) return;
			if (!Result) Self->SetError(Result.Message.empty()
				? "Texture2D reimport failed." : std::move(Result.Message));
			else
			{
				Self->SourceReferenceIndex.Invalidate();
				if (Result.SaveResult && !*Result.SaveResult)
					Self->SetError("Reimport succeeded; save failed: " + Result.SaveResult->Message);
			}
		});
	}

	auto MTextureEditor::ReimportFromFile(DTexture2D* Texture) -> void
	{
		if (!Texture || !Texture->GetPackage() || IsReadOnly()) return;
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Reimport Texture From File";
		Request.Filters = {
			{"All Supported Images", "*.png;*.jpg;*.jpeg;*.bmp;*.tga"},
			{"PNG", "*.png"}, {"JPEG", "*.jpg;*.jpeg"}, {"Bitmap", "*.bmp"},
			{"Targa", "*.tga"}};
		const FFileDialogResult Result = OpenFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return;
		if (Result.Status == EFileDialogStatus::Error)
		{
			SetError(Result.ErrorMessage);
			return;
		}
		const std::array Files{Result.FilePath};
		FReimportManager::ReimportFromFiles(*Texture, Files, {},
			[Weak = weak_from_this()](FReimportResult Reimported) {
				const auto Toolkit = Weak.lock();
				if (!Toolkit || !Toolkit->IsSessionActive()) return;
				auto* Self = static_cast<MTextureEditor*>(Toolkit.get());
				if (Reimported.Interrupted()) return;
				if (!Reimported) Self->SetError(Reimported.Message.empty()
					? "Texture2D reimport from file failed."
					: std::move(Reimported.Message));
				else
				{
					Self->SourceReferenceIndex.Invalidate();
					if (Reimported.SaveResult && !*Reimported.SaveResult)
						Self->SetError("Reimport succeeded; save failed: " + Reimported.SaveResult->Message);
				}
			});
	}

	auto MTextureEditor::DrawBuildSettings(DTexture2D* Texture) -> void
	{
		DrawDetailPanel(PropertyView, MakePropertyViewContext(), Texture, {
			.PropertyTableId = "TextureBuildSettings", .bShowEmptyMessage = false});
	}

	auto MTextureEditor::FinishActivePropertyEdit(bool bCancel) -> bool
	{
		const FPropertyViewContext Context = MakePropertyViewContext();
		return PropertyView.FinishActiveEdit(&Context, bCancel);
	}

	auto MTextureEditor::MakePropertyViewContext() -> FPropertyViewContext
	{
		return {
			.ReportError = [this](std::string Error) { SetError(std::move(Error)); },
			.bReadOnly = IsReadOnly(),
		};
	}

	auto MTextureEditor::SetError(std::string Message) -> void
	{
		ErrorMessage = std::move(Message);
		DURIN_ERROR("Texture editor: {}", ErrorMessage);
	}
}
