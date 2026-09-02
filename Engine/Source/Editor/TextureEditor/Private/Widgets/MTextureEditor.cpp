#include "Widgets/MTextureEditor.h"

#include "Asset/AssetCompilingManager.h"
#include "Asset/Asset.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "Dialogs/FileDialog.h"
#include "Editor/EditorEngine.h"
#include "Editor/WorkspaceManager.h"
#include "Editor/WorkspaceUI.h"
#include "MonaImGui.h"
#include "MonaImGuiPropertyTable.h"
#include "MonaImGuiWidgets.h"
#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "Misc/StringHelper.h"
#include "PixelFormat.h"
#include "Texture/Texture2D.h"
#include "Texture/TexturePayloadInspection.h"
#include "Texture/Texture2DCompilation.h"
#include "Texture/Texture2DRenderResource.h"
#include "EditorReimportHandler.h"
#include "Widgets/TexturePreview.h"
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

		auto DescribeBuildPhase(ETexture2DCompilationPhase Phase) -> const char*
		{
			switch (Phase)
			{
			case ETexture2DCompilationPhase::Queued: return "Queued";
			case ETexture2DCompilationPhase::Preparing: return "Preparing";
			case ETexture2DCompilationPhase::Building: return "Building";
			case ETexture2DCompilationPhase::Persisting: return "Persisting";
			case ETexture2DCompilationPhase::UploadPending: return "Upload Pending";
			case ETexture2DCompilationPhase::Ready: return "Ready";
			case ETexture2DCompilationPhase::Failed: return "Failed";
			case ETexture2DCompilationPhase::Cancelled: return "Cancelled";
			default: return "Not Submitted";
			}
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
			case ETexturePayloadRepairAction::RetryRuntimeResource: return "Retry runtime resource";
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

	MTextureEditor::MTextureEditor(::Durin::Editor::FWorkspaceManager& InWorkspaceManager)
		: WorkspaceManager(InWorkspaceManager)
	{
	}

	MTextureEditor::~MTextureEditor()
	{
		FinishActivePropertyEdit(true);
		for (auto& [ResourceId, Texture] : OpenTextures)
		{
			(void)ResourceId;
			if (Texture)
				FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Texture);
		}
	}

	auto MTextureEditor::GetWorkspaceType() const -> const ::Durin::Editor::FWorkspaceTypeId&
	{
		return Workspace::Type;
	}

	auto MTextureEditor::OpenDocument(const ::Durin::Editor::FDocumentTab& Document) -> ::Durin::Editor::EDocumentOpenResult
	{
		if (Document.ResourceId.empty()) return ::Durin::Editor::EDocumentOpenResult::Rejected;
		if (FindOpenTexture(Document.ResourceId)) return ::Durin::Editor::EDocumentOpenResult::Opened;
		FObjectPath AssetPath;
		std::string PathError;
		if (!FObjectPath::TryCreate(Document.ResourceId, AssetPath, &PathError))
		{
			SetError(std::move(PathError));
			return ::Durin::Editor::EDocumentOpenResult::Rejected;
		}
		DTexture2D* Texture = nullptr;
		const FAssetResult Result = LoadObject(AssetPath, Texture);
		if (!Result || !Texture)
		{
			SetError(Result ? "The selected asset is not a Texture2D." : Result.Message);
			return ::Durin::Editor::EDocumentOpenResult::Rejected;
		}
		OpenTextures.emplace(Document.ResourceId, Texture);
		PreviewStates.try_emplace(Document.ResourceId);
		return ::Durin::Editor::EDocumentOpenResult::Opened;
	}

	auto MTextureEditor::ActivateDocument(const ::Durin::Editor::FDocumentTab& Document) -> void
	{
		DTexture2D* Texture = FindOpenTexture(Document.ResourceId);
		if (PropertyView.IsEditing() && !PropertyView.IsEditingObject(Texture) && !FinishActivePropertyEdit(true)) return;
		Documents.Activate(Document, Texture);
	}

	auto MTextureEditor::RequestDeactivate() -> bool
	{
		return FinishActivePropertyEdit(true);
	}

	auto MTextureEditor::RequestCloseDocument(const ::Durin::Editor::FDocumentTab& Document) -> ::Durin::Editor::EDocumentCloseResult
	{
		if (PropertyView.IsEditingObject(FindOpenTexture(Document.ResourceId)) && !FinishActivePropertyEdit(true))
			return ::Durin::Editor::EDocumentCloseResult::Rejected;
		if (IsDocumentDirty(Document)) return ::Durin::Editor::EDocumentCloseResult::PendingConfirmation;
		if (DTexture2D* Texture = FindOpenTexture(Document.ResourceId))
			FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Texture);
		OpenTextures.erase(Document.ResourceId);
		PreviewStates.erase(Document.ResourceId);
		Documents.Close(Document.ResourceId);
		return ::Durin::Editor::EDocumentCloseResult::Closed;
	}

	auto MTextureEditor::SaveDocument(const ::Durin::Editor::FDocumentTab& Document) -> bool
	{
		return SaveTexture(FindOpenTexture(Document.ResourceId));
	}

	auto MTextureEditor::DiscardDocument(const ::Durin::Editor::FDocumentTab& Document) -> bool
	{
		DTexture2D* Texture = FindOpenTexture(Document.ResourceId);
		return Documents.Discard(Texture, [Texture] {
			FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Texture);
		});
	}

	auto MTextureEditor::IsDocumentDirty(const ::Durin::Editor::FDocumentTab& Document) const -> bool
	{
		return Documents.IsDirty(FindOpenTexture(Document.ResourceId));
	}

	auto MTextureEditor::CanSaveActiveDocument() const -> bool
	{
		return Documents.CanSave(GetActiveTexture());
	}

	auto MTextureEditor::SaveActiveDocument() -> bool
	{
		return SaveTexture(GetActiveTexture());
	}

	auto MTextureEditor::CanUndo() const -> bool
	{
		return Documents.CanUndo();
	}

	auto MTextureEditor::CanRedo() const -> bool
	{
		return Documents.CanRedo();
	}

	auto MTextureEditor::GetUndoDescription() const -> std::string_view
	{
		return Documents.GetUndoDescription();
	}

	auto MTextureEditor::GetRedoDescription() const -> std::string_view
	{
		return Documents.GetRedoDescription();
	}

	auto MTextureEditor::Undo() -> bool
	{
		return FinishActivePropertyEdit(false) && Documents.Undo();
	}

	auto MTextureEditor::Redo() -> bool
	{
		return FinishActivePropertyEdit(false) && Documents.Redo();
	}

	auto MTextureEditor::DrawWorkspace(bool bActive) -> bool
	{
		if (!bActive && PropertyView.IsEditing()) FinishActivePropertyEdit(true);
		return Documents.GetDocumentHost().DrawDocuments(
			WorkspaceManager,
			Workspace::Type,
			Workspace::RootKey,
			[this](const ::Durin::Editor::FDocumentTab& Document) {
				return FindOpenTexture(Document.ResourceId) != nullptr;
			},
			[this](const ::Durin::Editor::FDocumentTab& Document) {
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
		return Documents.Save(Texture, [this, Texture] {
			if (!HasPendingTexture2DCompilation(*Texture)) return true;
			SetError(
				"This texture has an uncommitted asynchronous build. "
				"Choose Wait for Build to commit it, or Cancel Build to save the last successful state.");
			return false;
		}, [this](std::string Message) { SetError(std::move(Message)); });
	}

	auto MTextureEditor::DrawDocument(const ::Durin::Editor::FDocumentTab& Document, DTexture2D* Texture) -> void
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

	auto MTextureEditor::DrawToolbar(const ::Durin::Editor::FDocumentTab& Document, DTexture2D* Texture) -> void
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

		const FTexture2DCompilationDiagnostic Diagnostic =
			GetTexture2DCompilationDiagnostic(*Texture);
		const bool bPending = HasPendingTexture2DCompilation(*Texture);
		const bool bReady = Texture->HasPlatformData();
		const std::string StatusName = bPending ? DescribeBuildPhase(Diagnostic.Phase)
			: bReady ? "CPU Ready" : "Not Built";
		const ImVec4 StatusColor = bReady
			? ImVec4(0.40f, 0.85f, 0.52f, 1.0f)
			: (!bPending
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
			if (ImGui::CollapsingHeader("Payload Lifecycle", ImGuiTreeNodeFlags_DefaultOpen))
				DrawPayloadLifecycle(Texture);
			if (ImGui::CollapsingHeader("Build Settings", ImGuiTreeNodeFlags_DefaultOpen))
				DrawBuildSettings(Texture);
			if (ImGui::CollapsingHeader("Source Information", ImGuiTreeNodeFlags_DefaultOpen))
				DrawSourceData(Texture);
		}
		ImGui::EndChild();
	}

	auto MTextureEditor::DrawBuildReadiness(DTexture2D* Texture) -> void
	{
		const FTexture2DCompilationDiagnostic Diagnostic =
			GetTexture2DCompilationDiagnostic(*Texture);
		if (Diagnostic.Phase == ETexture2DCompilationPhase::None
			|| Diagnostic.Phase == ETexture2DCompilationPhase::Ready) return;
		const bool bPending = HasPendingTexture2DCompilation(*Texture);
		const ImVec4 PhaseColor = Diagnostic.Phase == ETexture2DCompilationPhase::Failed
			? ImVec4(1.0f, 0.42f, 0.32f, 1.0f)
			: Diagnostic.Phase == ETexture2DCompilationPhase::Cancelled
				? ImVec4(0.75f, 0.75f, 0.75f, 1.0f)
				: ImVec4(0.42f, 0.72f, 1.0f, 1.0f);
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.11f, 0.16f, 0.65f));
		ImGui::BeginChild(
			"TextureBuildReadiness",
			ImVec2(0, 0),
			ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
		ImGui::TextColored(PhaseColor, "%s", DescribeBuildPhase(Diagnostic.Phase));
		ImGui::TextDisabled(
			"Request %llu  Request Serial %llu",
			static_cast<unsigned long long>(Diagnostic.RequestId),
			static_cast<unsigned long long>(Diagnostic.RequestSerial));
		if (Diagnostic.QueuedNanoseconds > 0)
			ImGui::Text("Queue: %.2f ms", Diagnostic.QueuedNanoseconds / 1'000'000.0);
		if (Diagnostic.WorkerNanoseconds > 0)
			ImGui::Text("Worker: %.2f ms", Diagnostic.WorkerNanoseconds / 1'000'000.0);
		ImGui::Text(
			"Estimated: %s  Decoded: %s  Peak intermediate: %s  Result: %s",
			StringUtils::FormatByteSize(Diagnostic.Metrics.EstimatedBytes).c_str(),
			StringUtils::FormatByteSize(Diagnostic.Metrics.DecodedBytes).c_str(),
			StringUtils::FormatByteSize(Diagnostic.Metrics.PeakIntermediateBytes).c_str(),
			StringUtils::FormatByteSize(Diagnostic.Metrics.ResultBytes).c_str());
		if (!Diagnostic.Message.empty())
			ImGui::TextWrapped("%s", Diagnostic.Message.c_str());
		if (Diagnostic.Phase == ETexture2DCompilationPhase::Failed
			&& Diagnostic.FailurePhase != ETexture2DCompilationPhase::None)
			ImGui::TextDisabled(
				"Failure stage: %s", DescribeBuildPhase(Diagnostic.FailurePhase));
		if (bPending)
		{
			if (ImGui::Button("Cancel Build"))
				FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Texture);
			ImGui::SameLine();
			if (ImGui::Button("Wait for Build"))
			{
				if (!WaitForTexture2DCompilation(*Texture))
				{
					const FTexture2DCompilationDiagnostic Completed =
						GetTexture2DCompilationDiagnostic(*Texture);
					SetError(Completed.Message.empty()
						? "The texture build did not complete." : Completed.Message);
				}
			}
		}
		ImGui::EndChild();
		ImGui::PopStyleColor();
		ImGui::Spacing();
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
		const FTexture2DCompilationDiagnostic Diagnostic =
			GetTexture2DCompilationDiagnostic(*Texture);
		const ETextureRenderFailure RenderFailure = Texture->GetRenderFailure();
		if (Diagnostic.Phase != ETexture2DCompilationPhase::Failed
			&& RenderFailure == ETextureRenderFailure::None) return;
		const char* Title = "Build Error";
		ImVec4 TitleColor(1.0f, 0.5f, 0.3f, 1.0f); // Amber default
		std::string Message = Diagnostic.Message;
		if (RenderFailure == ETextureRenderFailure::UnsupportedFormat)
		{
			Title = "Unsupported Format";
			TitleColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
			Message = "The selected pixel format is not supported by this GPU.";
		}
		else if (RenderFailure == ETextureRenderFailure::CreateOrUpload)
		{
			Title = "GPU Upload Failure";
			TitleColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
			Message = "The texture could not be created or uploaded to the GPU.";
		}
		else if (Message.empty())
		{
			Message = "The platform texture data could not be built.";
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

		if (RenderFailure == ETextureRenderFailure::CreateOrUpload)
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
		FTextureSourceData BuildInput = Texture->CreateBuildInput().ToSourceData();
		const FTextureSourceData* Source = BuildInput.IsValid()
			? &BuildInput : nullptr;
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
				const std::span<const ::Durin::Editor::FSourceReference> References =
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
			DrawInfoRow("Dimensions", FormatDimensions(Source.Width, Source.Height));
			DrawInfoRow("Source Channels", std::format("{}", Source.SourceChannelCount));
			DrawInfoRow("Transparency", Source.bHasTransparency ? "Present" : "Opaque");
			DrawInfoRow("Decoded Format", Source.Format == ETextureSourceFormat::RGBA8 ? "RGBA8" : "Invalid");
		}
		else
		{
			DrawInfoRow("Status", "Source data unavailable");
		}
		MonaImGui::PropertyEdit::EndTable();
		const bool bCanReimport = !SourceHint.empty();
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
	}

	auto MTextureEditor::ReimportSource(DTexture2D* Texture) -> void
	{
		if (!Texture) return;
		FReimportManager::Reimport(*Texture, {}, [this](FReimportResult Result) {
			if (!Result) SetError(Result.Message.empty()
				? "Texture2D reimport failed." : std::move(Result.Message));
			else SourceReferenceIndex.Invalidate();
		});
	}

	auto MTextureEditor::ReimportFromFile(DTexture2D* Texture) -> void
	{
		if (!Texture || !Texture->GetPackage()) return;
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
			[this](FReimportResult Reimported) {
				if (!Reimported) SetError(Reimported.Message.empty()
					? "Texture2D reimport from file failed."
					: std::move(Reimported.Message));
				else SourceReferenceIndex.Invalidate();
			});
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
			DrawInfoRow("CPU Status", "Platform data ready");
			DrawInfoRow("Pixel Format", GetPixelFormatInfo(Platform->PixelFormat).Name);
			DrawInfoRow("Mip Count", std::format("{}", Platform->Mips.size()));
			DrawInfoRow("Mip Range", std::format("{} to {}", FormatDimensions(Platform->Mips.front().Width, Platform->Mips.front().Height),
				FormatDimensions(Platform->Mips.back().Width, Platform->Mips.back().Height)));
			DrawInfoRow("Platform Bytes", StringUtils::FormatByteSize(TotalBytes));
			DrawInfoRow("Residency", "Fully resident");
		}
		else
		{
			DrawInfoRow("CPU Status", "Platform data unavailable");
		}

		DrawInfoRow("Build Revision", std::format("{}", Texture->GetBuildRevision()));

		DrawInfoRow("GPU State", GetEnumValueDisplayName(
			"Durin::ERenderResourceState",
			static_cast<uint64>(Texture->GetRenderResourceState())));

		MonaImGui::PropertyEdit::EndTable();
	}

	auto MTextureEditor::FinishActivePropertyEdit(bool bCancel) -> bool
	{
		const ::Durin::Editor::FPropertyViewContext Context = MakePropertyViewContext();
		return PropertyView.FinishActiveEdit(&Context, bCancel);
	}

	auto MTextureEditor::MakePropertyViewContext() -> ::Durin::Editor::FPropertyViewContext
	{
		return {
			.Transactor = GEditor ? GEditor->GetTransactor() : nullptr,
			.ReportError = [this](std::string Error) { SetError(std::move(Error)); },
		};
	}

	auto MTextureEditor::SetError(std::string Message) -> void
	{
		ErrorMessage = std::move(Message);
		DURIN_ERROR("Texture editor: {}", ErrorMessage);
	}
}
