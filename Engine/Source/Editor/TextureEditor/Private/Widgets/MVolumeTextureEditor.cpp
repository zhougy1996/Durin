#include "Widgets/MVolumeTextureEditor.h"

#include "AssetAuthoring.h"
#include "DObject/Package.h"
#include "Editor/WorkspaceManager.h"
#include "MonaCoreGlobals.h"
#include "MonaImGui.h"
#include "MonaImGuiPropertyTable.h"
#include "MonaUIBackend.h"
#include "Texture/TexturePayloadInspection.h"
#include "Texture/VolumeTexture.h"
#include "Workspace/VolumeTextureEditorWorkspace.h"

namespace Durin::Editor::Texture
{
	namespace
	{
		auto FormatBytes(uint64 Bytes) -> std::string
		{
			if (Bytes >= 1024ull * 1024ull)
				return std::format("{:.2f} MiB", static_cast<double>(Bytes) / (1024.0 * 1024.0));
			if (Bytes >= 1024ull)
				return std::format("{:.2f} KiB", static_cast<double>(Bytes) / 1024.0);
			return std::format("{} bytes", Bytes);
		}

		auto DrawFact(const char* Label, std::string_view Value) -> void
		{
			MonaImGui::PropertyEdit::BeginRow(Label, true);
			ImGui::TextWrapped("%.*s", static_cast<int>(Value.size()), Value.data());
			MonaImGui::PropertyEdit::EndRow(true);
		}

		auto PayloadStateText(ETexturePayloadState State) -> const char*
		{
			switch (State)
			{
			case ETexturePayloadState::Available: return "Available";
			case ETexturePayloadState::NotPresent: return "Not present";
			case ETexturePayloadState::Missing: return "Missing";
			case ETexturePayloadState::Stale: return "Stale";
			case ETexturePayloadState::Corrupt: return "Corrupt";
			case ETexturePayloadState::Unsupported: return "Unsupported";
			case ETexturePayloadState::Failed: return "Failed";
			case ETexturePayloadState::Unknown: return "Unknown";
			}
			return "Unknown";
		}

		auto PayloadRepairText(ETexturePayloadRepairAction Repair) -> const char*
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

		auto AxisSliceCount(const FVolumeTextureMipData& Mip,
			EVolumeTexturePreviewAxis Axis) -> uint32
		{
			return Axis == EVolumeTexturePreviewAxis::XY ? Mip.Depth
				: Axis == EVolumeTexturePreviewAxis::XZ ? Mip.Height : Mip.Width;
		}
	}

	MVolumeTextureEditor::MVolumeTextureEditor(
		::Durin::Editor::FWorkspaceManager& InManager) : Manager(InManager) {}

	auto MVolumeTextureEditor::GetWorkspaceType() const
		-> const ::Durin::Editor::FWorkspaceTypeId& { return VolumeWorkspace::Type; }

	auto MVolumeTextureEditor::OpenDocument(const ::Durin::Editor::FDocumentTab& Document)
		-> ::Durin::Editor::EDocumentOpenResult
	{
		if (Document.ResourceId.empty()) return ::Durin::Editor::EDocumentOpenResult::Rejected;
		if (Find(Document.ResourceId)) return ::Durin::Editor::EDocumentOpenResult::Opened;
		FAssetPath Path;
		std::string Error;
		if (!FAssetPath::TryCreate(Document.ResourceId, Path, &Error))
		{
			SetError(std::move(Error));
			return ::Durin::Editor::EDocumentOpenResult::Rejected;
		}
		DVolumeTexture* Texture = nullptr;
		const Asset::FAssetResult Result = Asset::LoadAsset(Path, Texture);
		if (!Result || !Texture)
		{
			SetError(Result ? "The selected asset is not a VolumeTexture." : Result.Message);
			return ::Durin::Editor::EDocumentOpenResult::Rejected;
		}
		OpenTextures.emplace(Document.ResourceId, Texture);
		PreviewStates.try_emplace(Document.ResourceId);
		return ::Durin::Editor::EDocumentOpenResult::Opened;
	}

	auto MVolumeTextureEditor::ActivateDocument(const ::Durin::Editor::FDocumentTab& Document) -> void
	{
		Documents.Activate(Document, Find(Document.ResourceId));
	}

	auto MVolumeTextureEditor::RequestCloseDocument(const ::Durin::Editor::FDocumentTab& Document)
		-> ::Durin::Editor::EDocumentCloseResult
	{
		if (IsDocumentDirty(Document))
			return ::Durin::Editor::EDocumentCloseResult::PendingConfirmation;
		OpenTextures.erase(Document.ResourceId);
		PreviewStates.erase(Document.ResourceId);
		Documents.Close(Document.ResourceId);
		return ::Durin::Editor::EDocumentCloseResult::Closed;
	}

	auto MVolumeTextureEditor::SaveDocument(const ::Durin::Editor::FDocumentTab& Document) -> bool
	{
		return Save(Find(Document.ResourceId));
	}

	auto MVolumeTextureEditor::DiscardDocument(const ::Durin::Editor::FDocumentTab& Document) -> bool
	{
		return Documents.Discard(Find(Document.ResourceId));
	}

	auto MVolumeTextureEditor::IsDocumentDirty(const ::Durin::Editor::FDocumentTab& Document) const -> bool
	{
		return Documents.IsDirty(Find(Document.ResourceId));
	}

	auto MVolumeTextureEditor::CanSaveActiveDocument() const -> bool
	{
		return Documents.CanSave(Active());
	}

	auto MVolumeTextureEditor::SaveActiveDocument() -> bool { return Save(Active()); }

	auto MVolumeTextureEditor::DrawWorkspace(bool) -> bool
	{
		return Documents.GetDocumentHost().DrawDocuments(Manager,
			VolumeWorkspace::Type, VolumeWorkspace::RootKey,
			[this](const auto& Document) { return Find(Document.ResourceId) != nullptr; },
			[this](const auto& Document) { DrawDocument(Document, Find(Document.ResourceId)); });
	}

	auto MVolumeTextureEditor::ResetLayout() -> void
	{
		for (auto& [Id, State] : PreviewStates)
		{
			(void)Id;
			State.Axis = EVolumeTexturePreviewAxis::XY;
			State.Channel = ETexturePreviewChannel::RGBA;
			State.Mip = 0;
			State.Slice = 0;
			State.Zoom = 0.0f;
		}
	}

	auto MVolumeTextureEditor::Find(std::string_view ResourceId) const -> DVolumeTexture*
	{
		const auto It = OpenTextures.find(std::string(ResourceId));
		return It == OpenTextures.end() ? nullptr : It->second.Get();
	}

	auto MVolumeTextureEditor::Active() const -> DVolumeTexture*
	{
		return Find(Documents.GetActiveResourceId());
	}

	auto MVolumeTextureEditor::Save(DVolumeTexture* Texture) -> bool
	{
		return Documents.Save(Texture, {}, [this](std::string Error) {
			SetError(std::move(Error));
		});
	}

	auto MVolumeTextureEditor::DrawDocument(const ::Durin::Editor::FDocumentTab& Document,
		DVolumeTexture* Texture) -> void
	{
		if (!Texture) return;
		Texture->RefreshBuildStatus();
		if (ImGui::Button("Save")) Save(Texture);
		ImGui::SameLine();
		if (ImGui::Button("Refresh"))
		{
			std::string Error;
			if (!Texture->PostLoad(Error)) SetError(std::move(Error));
		}
		ImGui::SameLine();
		ImGui::TextDisabled("%s", Document.ResourceId.c_str());
		ImGui::Separator();
		const ImVec2 Available = ImGui::GetContentRegionAvail();
		if (ImGui::BeginChild("VolumePreviewPane", ImVec2(Available.x * 0.68f, Available.y), ImGuiChildFlags_Borders))
			DrawPreview(Document.ResourceId, Texture);
		ImGui::EndChild();
		ImGui::SameLine();
		if (ImGui::BeginChild("VolumeDetailsPane", ImVec2(0.0f, Available.y), ImGuiChildFlags_Borders))
			DrawDetails(Texture);
		ImGui::EndChild();
		if (Documents.GetActiveResourceId() == Document.ResourceId)
			MonaImGui::ErrorDialog("Volume Texture Editor Error", ErrorMessage);
	}

	auto MVolumeTextureEditor::DrawPreview(const std::string& ResourceId,
		DVolumeTexture* Texture) -> void
	{
		auto& State = PreviewStates.try_emplace(ResourceId).first->second;
		const FVolumeTexturePlatformData* Platform = Texture->GetPlatformData();
		const FVolumeTextureSourceData& Source = Texture->GetSourceData();
		const bool bPlatform = Platform && Platform->IsValid()
			&& (Platform->PixelFormat == EPixelFormat::R8_UNORM
				|| Platform->PixelFormat == EPixelFormat::RGBA8_UNORM);
		const bool bSource = Source.IsValid()
			&& (Source.Format == EVolumeTextureFormat::R8_UNORM
				|| Source.Format == EVolumeTextureFormat::RGBA8_UNORM);
		ImGui::TextDisabled("VOLUME SLICE PREVIEW");
		constexpr std::array AxisLabels{"XY", "XZ", "YZ"};
		for (uint32 Index = 0; Index < AxisLabels.size(); ++Index)
		{
			if (Index) ImGui::SameLine();
			if (ImGui::RadioButton(AxisLabels[Index], static_cast<uint32>(State.Axis) == Index))
			{
				State.Axis = static_cast<EVolumeTexturePreviewAxis>(Index);
				State.Slice = 0;
			}
		}
		const uint32 MipCount = bPlatform ? static_cast<uint32>(Platform->Mips.size()) : 1u;
		State.Mip = MipCount ? std::min(State.Mip, MipCount - 1) : 0;
		if (bPlatform && MipCount > 1)
		{
			int Mip = static_cast<int>(State.Mip);
			if (ImGui::SliderInt("Mip", &Mip, 0, static_cast<int>(MipCount - 1), "%d",
				ImGuiSliderFlags_AlwaysClamp)) { State.Mip = static_cast<uint32>(Mip); State.Slice = 0; }
		}
		uint32 SliceCount = 0;
		if (bPlatform) SliceCount = AxisSliceCount(Platform->Mips[State.Mip], State.Axis);
		else if (bSource) SliceCount = State.Axis == EVolumeTexturePreviewAxis::XY ? Source.Depth
			: State.Axis == EVolumeTexturePreviewAxis::XZ ? Source.Height : Source.Width;
		if (SliceCount) State.Slice = std::min(State.Slice, SliceCount - 1);
		if (SliceCount > 1)
		{
			int Slice = static_cast<int>(State.Slice);
			if (ImGui::SliderInt("Slice", &Slice, 0, static_cast<int>(SliceCount - 1), "%d",
				ImGuiSliderFlags_AlwaysClamp)) State.Slice = static_cast<uint32>(Slice);
		}
		constexpr std::array ChannelLabels{"RGBA", "R", "G", "B", "A"};
		for (uint32 Index = 0; Index < ChannelLabels.size(); ++Index)
		{
			if (Index) ImGui::SameLine();
			const auto Channel = static_cast<ETexturePreviewChannel>(Index);
			if (ImGui::RadioButton(ChannelLabels[Index], State.Channel == Channel))
				State.Channel = Channel;
		}
		const uint64 SelectionKey = static_cast<uint64>(State.Mip)
			| (static_cast<uint64>(State.Slice) << 16)
			| (static_cast<uint64>(State.Axis) << 48)
			| (static_cast<uint64>(State.Channel) << 52)
			| (bPlatform ? 1ull << 60 : 0);
		if ((bPlatform || bSource) && (State.Revision != Texture->GetBuildRevision()
			|| State.SelectionKey != SelectionKey || !State.Preview->IsValid()))
		{
			FVolumeTexturePreviewSlice Slice = bPlatform
				? ExtractVolumeTexturePreviewSlice(Platform->Mips[State.Mip],
					Platform->PixelFormat, State.Axis, State.Slice)
				: ExtractVolumeTexturePreviewSlice(Source, State.Axis, State.Slice);
			if (Slice.IsValid()) State.Preview->UploadRGBA8(
				Slice.Width, Slice.Height, Slice.Pixels, State.Channel);
			else State.Preview->Release();
			State.Revision = Texture->GetBuildRevision();
			State.SelectionKey = SelectionKey;
		}
		if (!State.Preview->IsValid())
		{
			ImGui::Separator();
			ImGui::TextDisabled("R8/RGBA8 preview data is unavailable.");
			return;
		}
		ImGui::Separator();
		const ImVec2 Canvas = ImGui::GetContentRegionAvail();
		const float Scale = State.Zoom > 0.0f ? State.Zoom : std::clamp(std::min(
			(Canvas.x - 32.0f) / State.Preview->GetWidth(),
			(Canvas.y - 32.0f) / State.Preview->GetHeight()), 0.01f, 16.0f);
		if (ImGui::Button("Fit")) State.Zoom = 0.0f;
		ImGui::SameLine();
		if (ImGui::Button("1:1")) State.Zoom = 1.0f;
		const FVector2f Size(State.Preview->GetWidth() * Scale, State.Preview->GetHeight() * Scale);
		if (!Mona::GetActiveUIBackend()
			|| !Mona::GetActiveUIBackend()->DrawImage(State.Preview->GetTexture(), Size))
			ImGui::Dummy(ImVec2(Size.x, Size.y));
	}

	auto MVolumeTextureEditor::DrawDetails(DVolumeTexture* Texture) -> void
	{
		ImGui::TextDisabled("VOLUME DETAILS");
		if (!MonaImGui::PropertyEdit::BeginTable("VolumeTextureFacts")) return;
		const auto* Platform = Texture->GetPlatformData();
		const auto& Source = Texture->GetSourceData();
		DrawFact("Build status", std::to_string(static_cast<uint32>(Texture->GetBuildStatus())));
		if (Platform && Platform->IsValid())
		{
			const auto& Mip = Platform->Mips.front();
			DrawFact("Built dimensions", std::format("{} x {} x {}", Mip.Width, Mip.Height, Mip.Depth));
			DrawFact("Mip levels", std::to_string(Platform->Mips.size()));
			uint64 Bytes = 0;
			for (const auto& Entry : Platform->Mips) Bytes += Entry.Voxels.size();
			DrawFact("Built voxels", FormatBytes(Bytes));
		}
		if (Source.IsValid())
		{
			DrawFact("Source dimensions", std::format("{} x {} x {}", Source.Width, Source.Height, Source.Depth));
			DrawFact("Source voxels", FormatBytes(Source.GetVoxelBytes().size()));
		}
		const auto& Import = Texture->GetSourceImportData();
		DrawFact("Source file", Import.SourceFile.empty() ? "No source provenance" : Import.SourceFile);
		if (Import.HasSource())
			DrawFact("Atlas layout", std::format("{} x {} slices | {} x {} tiles | depth {}",
				Import.SliceWidth, Import.SliceHeight, Import.TilesX, Import.TilesY, Import.Depth));
		if (!Texture->GetLastBuildError().empty()) DrawFact("Diagnostic", Texture->GetLastBuildError());
		MonaImGui::PropertyEdit::EndTable();
		ImGui::Spacing();
		if (MonaImGui::PropertyEdit::BeginTable("VolumeTexturePayloadLifecycle"))
		{
			const FTexturePayloadInspection Inspection = InspectTexturePayloads(*Texture);
			for (const FTexturePayloadInspectionEntry& Entry : Inspection.Entries)
			{
				const char* Stage = Entry.Stage == ETexturePayloadStage::Source ? "Source payload"
					: Entry.Stage == ETexturePayloadStage::DerivedData ? "Derived payload"
					: Entry.Stage == ETexturePayloadStage::Cooked ? "Cooked payload"
					: Entry.Stage == ETexturePayloadStage::Decoded ? "Decoded payload"
					: "GPU resource";
				DrawFact(Stage, std::format("{} | {} | {} | repair: {}",
					PayloadStateText(Entry.State), Entry.Placement,
					FormatBytes(Entry.LogicalByteCount), PayloadRepairText(Entry.Repair)));
			}
			MonaImGui::PropertyEdit::EndTable();
		}
		ImGui::Spacing();
		ImGui::TextWrapped("Reimport and source repair remain available from the Content Browser import-record actions.");
	}
}
