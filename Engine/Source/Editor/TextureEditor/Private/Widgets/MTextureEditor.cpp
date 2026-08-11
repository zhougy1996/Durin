#include "Widgets/MTextureEditor.h"

#include "Asset/WorkspaceAssetOpenCompatibility.h"
#include "AssetImportCore.h"
#include "AssetSystem.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "Dialogs/FileDialog.h"
#include "Editor/EditorEngine.h"
#include "Editor/WorkspaceManager.h"
#include "Editor/WorkspaceUI.h"
#include "Misc/Paths.h"
#include "MonaImGui.h"
#include "MonaImGuiPropertyTable.h"
#include "MonaImGuiWidgets.h"
#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "PixelFormat.h"
#include "Source/MountedSourceRelocation.h"
#include "Source/SourcePath.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DBuildCoordinator.h"
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

		auto FindOwningMount(std::string_view VirtualPath)
			-> const PathUtilities::FMountPoint*
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(VirtualPath);
			return Lookup ? Lookup.Mount : nullptr;
		}

		auto FormatDimensions(uint32 Width, uint32 Height) -> std::string
		{
			return std::format("{} x {}", Width, Height);
		}

		auto DescribeMountOwner(PathUtilities::EMountOwner Owner) -> const char*
		{
			switch (Owner)
			{
			case PathUtilities::EMountOwner::Engine: return "Engine";
			case PathUtilities::EMountOwner::ActiveProject: return "Project";
			case PathUtilities::EMountOwner::Extension: return "Extension";
			case PathUtilities::EMountOwner::ExternalSources: return "External sources";
			case PathUtilities::EMountOwner::Test: return "Test";
			}
			return "Unknown";
		}

		auto FormatByteCount(uint64 Bytes) -> std::string
		{
			if (Bytes >= 1024 * 1024) return std::format("{:.2f} MiB", static_cast<double>(Bytes) / (1024.0 * 1024.0));
			if (Bytes >= 1024) return std::format("{:.2f} KiB", static_cast<double>(Bytes) / 1024.0);
			return std::format("{} bytes", Bytes);
		}

		auto DescribeBuildPhase(ETexture2DBuildPhase Phase) -> const char*
		{
			switch (Phase)
			{
			case ETexture2DBuildPhase::Queued: return "Queued";
			case ETexture2DBuildPhase::Decoding: return "Decoding";
			case ETexture2DBuildPhase::Building: return "Building";
			case ETexture2DBuildPhase::Persisting: return "Persisting";
			case ETexture2DBuildPhase::UploadPending: return "Upload Pending";
			case ETexture2DBuildPhase::Ready: return "Ready";
			case ETexture2DBuildPhase::Failed: return "Failed";
			case ETexture2DBuildPhase::Cancelled: return "Cancelled";
			default: return "Not Submitted";
			}
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

	MTextureEditor::MTextureEditor(Editor::FWorkspaceManager& InWorkspaceManager)
		: WorkspaceManager(InWorkspaceManager)
	{
	}

	MTextureEditor::~MTextureEditor()
	{
		FinishActivePropertyEdit(true);
		for (auto& [ResourceId, Texture] : OpenTextures)
		{
			(void)ResourceId;
			if (Texture) Texture->CancelPendingBuild();
		}
	}

	auto MTextureEditor::GetWorkspaceType() const -> const Editor::FWorkspaceTypeId&
	{
		return TextureEditorWorkspace::Type;
	}

	auto MTextureEditor::OpenDocument(const Editor::FDocumentTab& Document) -> Editor::EDocumentOpenResult
	{
		if (Document.ResourceId.empty()) return Editor::EDocumentOpenResult::Rejected;
		if (FindOpenTexture(Document.ResourceId)) return Editor::EDocumentOpenResult::Opened;
		FAssetPath AssetPath;
		std::string PathError;
		if (!FAssetPath::TryCreate(Document.ResourceId, AssetPath, &PathError))
		{
			SetError(std::move(PathError));
			return Editor::EDocumentOpenResult::Rejected;
		}
		Editor::FWorkspaceAssetOpenCompatibility CompatibilityPolicy(AssetPath);
		DTexture2D* Texture = nullptr;
		Asset::FAssetLoadReport LoadReport;
		const Asset::FAssetResult Result = Asset::LoadAsset(AssetPath, Texture, &LoadReport);
		if (!Result || !Texture)
		{
			SetError(Result ? "The selected asset is not a Texture2D." : Result.Message);
			return Editor::EDocumentOpenResult::Rejected;
		}
		std::string CompatibilityDiagnostic;
		if (CompatibilityPolicy.RejectIfIncompatible(LoadReport, CompatibilityDiagnostic))
		{
			SetError(std::move(CompatibilityDiagnostic));
			return Editor::EDocumentOpenResult::Rejected;
		}
		OpenTextures.emplace(Document.ResourceId, Texture);
		PreviewStates.try_emplace(Document.ResourceId);
		return Editor::EDocumentOpenResult::Opened;
	}

	auto MTextureEditor::ActivateDocument(const Editor::FDocumentTab& Document) -> void
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

	auto MTextureEditor::RequestCloseDocument(const Editor::FDocumentTab& Document) -> Editor::EDocumentCloseResult
	{
		if (PropertyView.IsEditingObject(FindOpenTexture(Document.ResourceId)) && !FinishActivePropertyEdit(true))
			return Editor::EDocumentCloseResult::Rejected;
		if (IsDocumentDirty(Document)) return Editor::EDocumentCloseResult::PendingConfirmation;
		if (DTexture2D* Texture = FindOpenTexture(Document.ResourceId))
			Texture->CancelPendingBuild();
		OpenTextures.erase(Document.ResourceId);
		PreviewStates.erase(Document.ResourceId);
		if (ActiveResourceId == Document.ResourceId) ActiveResourceId.clear();
		return Editor::EDocumentCloseResult::Closed;
	}

	auto MTextureEditor::SaveDocument(const Editor::FDocumentTab& Document) -> bool
	{
		return SaveTexture(FindOpenTexture(Document.ResourceId));
	}

	auto MTextureEditor::DiscardDocument(const Editor::FDocumentTab& Document) -> bool
	{
		DTexture2D* Texture = FindOpenTexture(Document.ResourceId);
		if (!Texture || !Texture->GetPackage()) return false;
		Texture->CancelPendingBuild();
		Texture->GetPackage()->ClearDirty();
		return true;
	}

	auto MTextureEditor::IsDocumentDirty(const Editor::FDocumentTab& Document) const -> bool
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
			[this](const Editor::FDocumentTab& Document) {
				return FindOpenTexture(Document.ResourceId) != nullptr;
			},
			[this](const Editor::FDocumentTab& Document) {
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
		if (Texture->HasPendingBuild())
		{
			SetError(
				"This texture has an uncommitted asynchronous build. "
				"Choose Wait for Build to commit it, or Cancel Build to save the last successful state.");
			return false;
		}
		const Asset::FAssetResult Result = Asset::SavePackage(Texture->GetPackage());
		if (!Result)
		{
			SetError(Result.Message);
			return false;
		}
		return true;
	}

	auto MTextureEditor::DrawDocument(const Editor::FDocumentTab& Document, DTexture2D* Texture) -> void
	{
		Texture->RefreshBuildStatus();

		DrawToolbar(Document, Texture);
		ImGui::Spacing();

		if (ImGui::GetContentRegionAvail().x >= MonaImGui::ScaleUI(WideLayoutMinimumWidth))
			DrawWideLayout(Document.ResourceId, Texture);
		else
			DrawNarrowLayout(Document.ResourceId, Texture);

		if (ActiveResourceId != Document.ResourceId) return;
		MonaImGui::ErrorDialog("Texture Editor Error", ErrorMessage);
	}

	auto MTextureEditor::DrawToolbar(const Editor::FDocumentTab& Document, DTexture2D* Texture) -> void
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
			DrawBuildReadiness(Texture);
			DrawFailureState(Texture);
			if (ImGui::CollapsingHeader("Build Settings", ImGuiTreeNodeFlags_DefaultOpen))
				DrawBuildSettings(Texture);
			if (ImGui::CollapsingHeader("Source Information", ImGuiTreeNodeFlags_DefaultOpen))
				DrawSourceData(Texture);
		}
		ImGui::EndChild();
	}

	auto MTextureEditor::DrawBuildReadiness(DTexture2D* Texture) -> void
	{
		const FTexture2DBuildDiagnostic Diagnostic =
			Texture->GetBuildReadinessDiagnostic();
		if (Diagnostic.Phase == ETexture2DBuildPhase::None
			|| Diagnostic.Phase == ETexture2DBuildPhase::Ready) return;
		const bool bPending = Texture->HasPendingBuild();
		const ImVec4 PhaseColor = Diagnostic.Phase == ETexture2DBuildPhase::Failed
			? ImVec4(1.0f, 0.42f, 0.32f, 1.0f)
			: Diagnostic.Phase == ETexture2DBuildPhase::Cancelled
				? ImVec4(0.75f, 0.75f, 0.75f, 1.0f)
				: ImVec4(0.42f, 0.72f, 1.0f, 1.0f);
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.11f, 0.16f, 0.65f));
		ImGui::BeginChild(
			"TextureBuildReadiness",
			ImVec2(0, 0),
			ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
		ImGui::TextColored(PhaseColor, "%s", DescribeBuildPhase(Diagnostic.Phase));
		ImGui::TextDisabled(
			"Request %llu  Generation %llu",
			static_cast<unsigned long long>(Diagnostic.RequestId),
			static_cast<unsigned long long>(Diagnostic.Generation));
		if (Diagnostic.QueuedNanoseconds > 0)
			ImGui::Text("Queue: %.2f ms", Diagnostic.QueuedNanoseconds / 1'000'000.0);
		if (Diagnostic.WorkerNanoseconds > 0)
			ImGui::Text("Worker: %.2f ms", Diagnostic.WorkerNanoseconds / 1'000'000.0);
		ImGui::Text(
			"Estimated: %s  Decoded: %s  Peak intermediate: %s  Result: %s",
			FormatByteCount(Diagnostic.Metrics.EstimatedBytes).c_str(),
			FormatByteCount(Diagnostic.Metrics.DecodedBytes).c_str(),
			FormatByteCount(Diagnostic.Metrics.PeakIntermediateBytes).c_str(),
			FormatByteCount(Diagnostic.Metrics.ResultBytes).c_str());
		if (!Diagnostic.Message.empty())
			ImGui::TextWrapped("%s", Diagnostic.Message.c_str());
		if (Diagnostic.Phase == ETexture2DBuildPhase::Failed
			&& Diagnostic.FailurePhase != ETexture2DBuildPhase::None)
			ImGui::TextDisabled(
				"Failure stage: %s", DescribeBuildPhase(Diagnostic.FailurePhase));
		if (bPending)
		{
			if (ImGui::Button("Cancel Build")) Texture->CancelPendingBuild();
			ImGui::SameLine();
			if (ImGui::Button("Wait for Build"))
			{
				if (!Texture->WaitForPendingBuild())
					SetError(Texture->GetLastBuildError().empty()
						? "The texture build did not complete." : Texture->GetLastBuildError());
			}
		}
		ImGui::EndChild();
		ImGui::PopStyleColor();
		ImGui::Spacing();
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

		if (Status == ETextureBuildStatus::UploadFailure)
		{
			ImGui::SameLine();
			const ERenderResourceState RState =
				Texture->GetRenderResourceState();
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
			const bool bChannelChanged = PreviewState.SelectedChannel != PreviewState.LastAppliedChannel;
			if (bRevisionChanged || bMipChanged || bPreviewModeChanged || !Preview.IsValid())
			{
				if (PreviewState.bPreviewSource)
					Preview.UploadSource(*Source, PreviewState.SelectedChannel);
				else
					Preview.Upload(*Platform, PreviewState.SelectedMipIndex, PreviewState.SelectedChannel);
				PreviewState.LastUploadedMipIndex = PreviewState.SelectedMipIndex;
				PreviewState.LastObservedRevision = Texture->GetBuildRevision();
				PreviewState.bLastUploadWasSource = PreviewState.bPreviewSource;
				PreviewState.LastAppliedChannel = PreviewState.SelectedChannel;
			}
			else if (bChannelChanged)
			{
				Preview.SetChannel(PreviewState.SelectedChannel);
				PreviewState.LastAppliedChannel = PreviewState.SelectedChannel;
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
				if (Mona::GActiveUIBackend)
					bDrewPreview = Mona::GActiveUIBackend->DrawImage(
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
		SourceReferenceIndex.Refresh();
		if (!MonaImGui::PropertyEdit::BeginTable("TextureSourceData")) return;
		DrawInfoRow("Asset Destination",
			Texture->GetPackage() ? Texture->GetPackage()->GetPackagePath() : "");
		DrawInfoRow("Source Virtual Path", Texture->GetSourceFile());
		const FTextureSourceDiagnostic SourceDiagnostic = Texture->InspectSource();
		switch (SourceDiagnostic.Status)
		{
		case ETextureSourceStatus::Available:
			DrawInfoRow("Provenance", "Portable source available");
			break;
		case ETextureSourceStatus::Changed:
			DrawInfoRow("Provenance", "Mounted source bytes changed");
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
		if (!Texture->GetSourceFile().empty())
		{
			const PathUtilities::FSourcePathResult Resolved =
				PathUtilities::ResolveSourcePath(
					Texture->GetSourceFile(),
					PathUtilities::EPathExistence::AllowMissing);
			if (Resolved)
			{
				DrawInfoRow("Source Mount", std::format(
					"{} ({})  |  {}",
					Resolved.Mount->VirtualRoot,
					DescribeMountOwner(Resolved.Mount->Owner),
					Resolved.Mount->bAuthoringWritable ? "writable" : "read-only"));
			}
			const std::span<const Editor::FSourceReference> References =
				SourceReferenceIndex.FindReferences(Texture->GetSourceFile());
			DrawInfoRow("Shared References", std::format("{} asset(s)", References.size()));
		}
		if (!SourceDiagnostic.Message.empty())
			DrawInfoRow("Diagnostic", SourceDiagnostic.Message);
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
		const AssetImport::FSingleAssetCapabilitySet ImportCapabilities =
			AssetImport::QuerySingleAssetCapabilities(
				*Texture, AssetImport::GetProviderRegistry(),
				AssetImport::GetSingleAssetHandlerRegistry());
		const AssetImport::FSingleAssetCapability* ReimportCapability =
			ImportCapabilities.Find(
				AssetImport::ESingleAssetImportCapability::ReimportCurrentSource);
		const bool bCanReimport = ReimportCapability && ReimportCapability->bAvailable;
		const char* ReimportLabel = ReimportCapability
			&& !ReimportCapability->Label.empty()
			? ReimportCapability->Label.c_str()
			: "Reimport from Current Source";
		if (!bCanReimport) ImGui::BeginDisabled();
		if (ImGui::Button(ReimportLabel))
			ReimportSource(Texture);
		if (!bCanReimport) ImGui::EndDisabled();
		if (ImGui::IsItemHovered() && ReimportCapability)
			ImGui::SetTooltip("%s", bCanReimport
				? ReimportCapability->ReplacedStateDescription.c_str()
				: (ReimportCapability->Diagnostics.empty()
					? "Reimport is unavailable."
					: ReimportCapability->Diagnostics.back().Message.c_str()));
		ImGui::SameLine();
		if (ImGui::Button("Reference Existing...")) ChangeSourceReference(Texture);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(
				"Changes this asset to an existing mounted source without copying it.");
		ImGui::SameLine();
		if (ImGui::Button("Ingest External...")) IngestExternalSource(Texture);
		if (SourceDiagnostic.Status == ETextureSourceStatus::Missing
			|| SourceDiagnostic.Status == ETextureSourceStatus::Invalid)
		{
			ImGui::SameLine();
			if (ImGui::Button("Repair Source...")) RepairSource(Texture);
		}
		if (!Texture->GetSourceFile().empty())
		{
			if (ImGui::Button("Replace Shared Source..."))
				RequestSharedSourceReplacement(Texture);
			ImGui::SameLine();
			if (ImGui::Button("Create Private Copy..."))
				ChangeSourceLocation(Texture);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(
					"Copies the current source to a new mounted path and changes only this asset. "
					"The old shared source remains in place.");
			ImGui::SameLine();
			if (ImGui::Button("Relocate Shared Source..."))
				RequestSharedSourceRelocation(Texture);
		}
		DrawSharedSourceReplacementConfirmation(Texture);
		DrawSharedSourceRelocationConfirmation(Texture);
	}

	auto MTextureEditor::ReimportSource(DTexture2D* Texture) -> void
	{
		if (!Texture) return;
		std::string Error;
		if (!Texture->ReimportSource({}, Error)) SetError(std::move(Error));
	}

	auto MTextureEditor::ChangeSourceReference(DTexture2D* Texture) -> void
	{
		if (!Texture || !Texture->GetPackage()) return;
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Reference Existing Mounted Texture Source";
		Request.Filters = {
			{"All Supported Images", "*.png;*.jpg;*.jpeg;*.bmp;*.tga"},
			{"PNG", "*.png"}, {"JPEG", "*.jpg;*.jpeg"}, {"Bitmap", "*.bmp"},
			{"Targa", "*.tga"}};
		const FTextureSourceDiagnostic Existing = Texture->InspectSource();
		if (!Existing.PhysicalPath.empty())
			Request.InitialDirectory =
				std::filesystem::path(Existing.PhysicalPath).parent_path().generic_string();
		const FFileDialogResult Result = OpenFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return;
		if (Result.Status == EFileDialogStatus::Error)
		{
			SetError(Result.ErrorMessage);
			return;
		}
		const PathUtilities::FSourcePathResult Classified =
			PathUtilities::ClassifySourcePath(Result.FilePath);
		if (!Classified)
		{
			SetError(Classified.Error == PathUtilities::EMountPathError::UnknownMount
				? "The selected file is external. Use Ingest External Source instead."
				: Classified.Message);
			return;
		}
		std::string Error;
		if (!Texture->ChangeSourceReference(Classified.NormalizedVirtualPath, Error))
		{
			SetError(std::move(Error));
			return;
		}
		SourceReferenceIndex.Invalidate();
	}

	auto MTextureEditor::IngestExternalSource(DTexture2D* Texture) -> void
	{
		if (!Texture || !Texture->GetPackage()) return;
		const PathUtilities::FMountPoint* Mount =
			FindOwningMount(Texture->GetPackage()->GetPackagePath());
		if (!Mount)
		{
			SetError("The texture does not use a registered mount.");
			return;
		}

		FFileDialogRequest InputRequest;
		InputRequest.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		InputRequest.Title = "Select External Texture Source";
		InputRequest.Filters = {
			{"All Supported Images", "*.png;*.jpg;*.jpeg;*.bmp;*.tga"},
			{"PNG", "*.png"}, {"JPEG", "*.jpg;*.jpeg"}, {"Bitmap", "*.bmp"},
			{"Targa", "*.tga"}};
		const FFileDialogResult Input = OpenFileDialog(InputRequest);
		if (Input.Status == EFileDialogStatus::Cancelled) return;
		if (Input.Status == EFileDialogStatus::Error)
		{
			SetError(Input.ErrorMessage);
			return;
		}
		if (PathUtilities::ClassifySourcePath(Input.FilePath))
		{
			SetError("The selected source is already mounted. Use Reference Existing Source instead.");
			return;
		}

		FFileDialogRequest DestinationRequest;
		DestinationRequest.ParentWindowHandle =
			ImGui::GetMainViewport()->PlatformHandleRaw;
		DestinationRequest.Title = "Choose Mounted Texture Source Destination";
		DestinationRequest.Filters = InputRequest.Filters;
		DestinationRequest.InitialDirectory =
			(Mount->GetContentDir() / "Textures").generic_string();
		DestinationRequest.DefaultFileName =
			std::filesystem::path(Input.FilePath).filename().generic_string();
		const FFileDialogResult Destination = SaveFileDialog(DestinationRequest);
		if (Destination.Status == EFileDialogStatus::Cancelled) return;
		if (Destination.Status == EFileDialogStatus::Error)
		{
			SetError(Destination.ErrorMessage);
			return;
		}
		const PathUtilities::FSourcePathResult ClassifiedDestination =
			PathUtilities::ClassifySourcePath(Destination.FilePath);
		if (!ClassifiedDestination)
		{
			SetError(ClassifiedDestination.Message);
			return;
		}
		std::string Error;
		if (!Texture->IngestAndChangeSource(
			Input.FilePath, ClassifiedDestination.NormalizedVirtualPath, Error))
		{
			SetError(std::move(Error));
			return;
		}
		SourceReferenceIndex.Invalidate();
	}

	auto MTextureEditor::RepairSource(DTexture2D* Texture) -> void
	{
		if (!Texture) return;
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Repair Texture Source Reference";
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
		std::string Error;
		if (!Texture->RepairSourcePath(Result.FilePath, Error))
		{
			SetError(std::move(Error));
			return;
		}
		SourceReferenceIndex.Invalidate();
	}

	auto MTextureEditor::RequestSharedSourceReplacement(DTexture2D* Texture) -> void
	{
		if (!Texture || !Texture->GetPackage() || Texture->GetSourceFile().empty()) return;
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Select Replacement Bytes for Shared Texture Source";
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
		SourceReferenceIndex.Refresh();
		const std::span<const Editor::FSourceReference> References =
			SourceReferenceIndex.FindReferences(Texture->GetSourceFile());
		PendingSourceReplacement = {
			.SourceVirtualPath = Texture->GetSourceFile(),
			.ReplacementPhysicalPath = Result.FilePath,
			.AffectedAssets = {References.begin(), References.end()},
			.bOpenRequested = true};
	}

	auto MTextureEditor::DrawSharedSourceReplacementConfirmation(
		DTexture2D* Texture) -> void
	{
		if (PendingSourceReplacement.bOpenRequested)
		{
			ImGui::OpenPopup("Replace Shared Source");
			PendingSourceReplacement.bOpenRequested = false;
		}
		if (!ImGui::BeginPopupModal("Replace Shared Source", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
			return;
		ImGui::TextWrapped(
			"This mutates the shared mounted source. The current texture is reimported and saved immediately.");
		ImGui::TextUnformatted("Source virtual path");
		ImGui::TextDisabled("%s", PendingSourceReplacement.SourceVirtualPath.c_str());
		ImGui::TextUnformatted(std::format(
			"Affected assets ({})", PendingSourceReplacement.AffectedAssets.size()).c_str());
		ImGui::BeginChild("AffectedMountedSources",
			ImVec2(MonaImGui::ScaleUI(520.0f), MonaImGui::ScaleUI(130.0f)),
			ImGuiChildFlags_Borders);
		for (const Editor::FSourceReference& Reference :
			PendingSourceReplacement.AffectedAssets)
			ImGui::TextUnformatted(Reference.AssetPath.ToString().c_str());
		ImGui::EndChild();
		if (PendingSourceReplacement.AffectedAssets.size() > 1)
		{
			ImGui::TextWrapped(
				"Other assets retain their previous import hash and will report Changed Source until reimported.");
		}
		if (!SourceReferenceIndex.GetWarning().empty())
			ImGui::TextWrapped("%s", SourceReferenceIndex.GetWarning().c_str());
		const bool bImpactComplete = SourceReferenceIndex.GetWarning().empty();
		ImGui::BeginDisabled(!bImpactComplete);
		if (ImGui::Button("Replace and Reimport Current",
			ImVec2(MonaImGui::ScaleUI(220.0f), 0.0f)))
		{
			FMountedSourceReplacement Replacement;
			std::string Error;
			if (!PrepareMountedSourceReplacement(
				PendingSourceReplacement.ReplacementPhysicalPath,
				Texture->GetPackage()->GetPackagePath(),
				PendingSourceReplacement.SourceVirtualPath,
				Replacement, Error))
			{
				SetError(std::move(Error));
			}
			else if (!Texture->ReimportSource({}, Error) || !SaveTexture(Texture))
			{
				RollbackMountedSourceReplacement(Replacement);
				std::string RestoreError;
				Texture->ReimportSource({}, RestoreError);
				if (!Error.empty()) SetError(std::move(Error));
			}
			else
			{
				CommitMountedSourceReplacement(Replacement);
				SourceReferenceIndex.Invalidate();
			}
			PendingSourceReplacement = {};
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (MonaImGui::DialogButton("Cancel", true))
		{
			PendingSourceReplacement = {};
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	auto MTextureEditor::RequestSharedSourceRelocation(
		DTexture2D* Texture) -> void
	{
		if (!Texture || !Texture->GetPackage() || Texture->GetSourceFile().empty()) return;
		const PathUtilities::FSourcePathResult Existing =
			PathUtilities::ResolveSourcePath(
				Texture->GetSourceFile(),
				PathUtilities::EPathExistence::RequireFile);
		if (!Existing)
		{
			SetError(Existing.Message);
			return;
		}
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Choose Shared Texture Source Relocation";
		Request.Filters = {
			{"All Supported Images", "*.png;*.jpg;*.jpeg;*.bmp;*.tga"},
			{"PNG", "*.png"}, {"JPEG", "*.jpg;*.jpeg"}, {"Bitmap", "*.bmp"},
			{"Targa", "*.tga"}};
		Request.InitialDirectory =
			Existing.PhysicalPath.parent_path().generic_string();
		Request.DefaultFileName =
			Existing.PhysicalPath.filename().generic_string();
		const FFileDialogResult Result = SaveFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return;
		if (Result.Status == EFileDialogStatus::Error)
		{
			SetError(Result.ErrorMessage);
			return;
		}
		const PathUtilities::FSourcePathResult Destination =
			PathUtilities::ClassifySourcePath(Result.FilePath);
		if (!Destination)
		{
			SetError(Destination.Message);
			return;
		}
		if (Existing.PhysicalPath.extension() != Destination.PhysicalPath.extension())
		{
			SetError("Source relocation must preserve the file extension.");
			return;
		}
		SourceReferenceIndex.Refresh();
		const std::span<const Editor::FSourceReference> References =
			SourceReferenceIndex.FindReferences(Texture->GetSourceFile());
		PendingSourceRelocation = {
			.OriginalSourceVirtualPath = Texture->GetSourceFile(),
			.DestinationSourceVirtualPath = Destination.NormalizedVirtualPath,
			.AffectedAssets = {References.begin(), References.end()},
			.bOpenRequested = true};
	}

	auto MTextureEditor::DrawSharedSourceRelocationConfirmation(
		DTexture2D* Texture) -> void
	{
		if (PendingSourceRelocation.bOpenRequested)
		{
			ImGui::OpenPopup("Relocate Shared Source");
			PendingSourceRelocation.bOpenRequested = false;
		}
		if (!ImGui::BeginPopupModal("Relocate Shared Source", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
			return;
		ImGui::TextWrapped(
			"The source is copied first, every affected package is updated and saved, "
			"then the original is removed. Any failure restores package bytes and removes the staged copy.");
		ImGui::TextUnformatted("Original");
		ImGui::TextDisabled("%s",
			PendingSourceRelocation.OriginalSourceVirtualPath.c_str());
		ImGui::TextUnformatted("Destination");
		ImGui::TextDisabled("%s",
			PendingSourceRelocation.DestinationSourceVirtualPath.c_str());
		ImGui::TextUnformatted(std::format(
			"Affected assets ({})",
			PendingSourceRelocation.AffectedAssets.size()).c_str());
		ImGui::BeginChild("RelocatedMountedSources",
			ImVec2(MonaImGui::ScaleUI(520.0f), MonaImGui::ScaleUI(130.0f)),
			ImGuiChildFlags_Borders);
		for (const Editor::FSourceReference& Reference :
			PendingSourceRelocation.AffectedAssets)
			ImGui::TextUnformatted(Reference.AssetPath.ToString().c_str());
		ImGui::EndChild();
		if (!SourceReferenceIndex.GetWarning().empty())
			ImGui::TextWrapped("%s", SourceReferenceIndex.GetWarning().c_str());
		const bool bCanRelocate =
			!PendingSourceRelocation.AffectedAssets.empty()
			&& SourceReferenceIndex.GetWarning().empty();
		ImGui::BeginDisabled(!bCanRelocate);
		if (ImGui::Button("Relocate All References",
			ImVec2(MonaImGui::ScaleUI(210.0f), 0.0f)))
		{
			std::string Error;
			if (!Editor::RelocateMountedSourceAcrossPackages({
					.AuthoringAssetPath =
						Texture && Texture->GetPackage()
							? Texture->GetPackage()->GetPackagePath() : "",
					.OriginalSourceVirtualPath =
						PendingSourceRelocation.OriginalSourceVirtualPath,
					.DestinationSourceVirtualPath =
						PendingSourceRelocation.DestinationSourceVirtualPath,
					.AffectedAssets = PendingSourceRelocation.AffectedAssets},
				Error))
			{
				SetError(std::move(Error));
			}
			else
			{
				SourceReferenceIndex.Invalidate();
			}
			PendingSourceRelocation = {};
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (MonaImGui::DialogButton("Cancel", true))
		{
			PendingSourceRelocation = {};
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	auto MTextureEditor::ChangeSourceLocation(DTexture2D* Texture) -> void
	{
		if (!Texture || !Texture->GetPackage()) return;
		const PathUtilities::FMountPoint* Mount =
			FindOwningMount(Texture->GetPackage()->GetPackagePath());
		if (!Mount)
		{
			SetError("The texture is not inside a package-enabled mount.");
			return;
		}
		const FTextureSourceDiagnostic Diagnostic = Texture->InspectSource();
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Choose Texture Source Location";
		Request.Filters = {
			{"All Supported Images", "*.png;*.jpg;*.jpeg;*.bmp;*.tga"},
			{"PNG", "*.png"}, {"JPEG", "*.jpg;*.jpeg"}, {"Bitmap", "*.bmp"},
			{"Targa", "*.tga"}
		};
		Request.InitialDirectory = !Diagnostic.PhysicalPath.empty()
			? std::filesystem::path(Diagnostic.PhysicalPath).parent_path().generic_string()
			: (Mount->GetContentDir() / "Textures").generic_string();
		Request.DefaultFileName = !Texture->GetSourceFile().empty()
			? std::filesystem::path(Texture->GetSourceFile()).filename().generic_string()
			: "Texture.png";
		const FFileDialogResult Result = SaveFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return;
		if (Result.Status == EFileDialogStatus::Error)
		{
			SetError(Result.ErrorMessage);
			return;
		}
		const PathUtilities::FSourcePathResult Classified =
			PathUtilities::ClassifySourcePath(Result.FilePath);
		if (!Classified || Classified.Mount != Mount)
		{
			SetError("Texture source must stay inside its owning mount.");
			return;
		}
		const std::string SourceDestination = Classified.RelativePath.generic_string();
		std::string Error;
		if (!Texture->ChangeSourceLocation(SourceDestination, Error))
			SetError(std::move(Error));
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

		DrawInfoRow("GPU State", GetEnumValueDisplayName(
			"Durin::ERenderResourceState",
			static_cast<uint64>(Texture->GetRenderResourceState())));

		MonaImGui::PropertyEdit::EndTable();
	}

	auto MTextureEditor::FinishActivePropertyEdit(bool bCancel) -> bool
	{
		const Editor::FPropertyViewContext Context = MakePropertyViewContext();
		return PropertyView.FinishActiveEdit(&Context, bCancel);
	}

	auto MTextureEditor::MakePropertyViewContext() -> Editor::FPropertyViewContext
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
