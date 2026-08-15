#include "Widgets/MSkeletalAssetInspector.h"

#include "Animation/AnimationClip.h"
#include "Asset/WorkspaceAssetOpenCompatibility.h"
#include "AssetLoad.h"
#include "ImportRecordIndex.h"
#include "DObject/Object.h"
#include "Editor/WorkspaceManager.h"
#include "Materials/MaterialInterface.h"
#include "MonaImGui.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "SkeletalMesh/Skeleton.h"
#include "Workspace/SkeletalMeshEditorWorkspace.h"

namespace Durin::Editor::SkeletalMesh
{
	namespace
	{
		auto DrawInfoRow(const char* Label, std::string_view Value) -> void
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextDisabled("%s", Label);
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(Value.data(), Value.data() + Value.size());
		}

		auto TrackPathLabel(EAnimationTrackPath Path) -> const char*
		{
			switch (Path)
			{
			case EAnimationTrackPath::Translation: return "Translation";
			case EAnimationTrackPath::Rotation: return "Rotation";
			case EAnimationTrackPath::Scale: return "Scale";
			}
			return "Unknown";
		}

		auto ResolveSameRecordPreviewPeer(const FAssetPath& AssetPath, DObject* Asset,
			std::vector<std::string>& OutPaths)
			-> DObject*
		{
			const Asset::Import::FImportRecordInspection Inspection =
				Asset::Import::InspectImportRecordForOutput(
					AssetPath, Asset::Import::GetImportRecordIndex());
			if (!Inspection || !Inspection.Record) return nullptr;
			std::vector<const Asset::Import::FImportRecordOutput*> Candidates;
			for (const Asset::Import::FImportRecordOutput& Output : Inspection.Record->GetOutputs())
			{
				const bool bWanted = Cast<DSkeletalMesh>(Asset)
					? Output.AssetClassName == DAnimationClip::StaticClass()->GetQualifiedName().ToString()
					: Cast<DAnimationClip>(Asset)
						&& Output.AssetClassName == DSkeletalMesh::StaticClass()->GetQualifiedName().ToString();
				if (bWanted && Output.Policy == Asset::Import::EImportRecordOutputPolicy::Managed)
					Candidates.push_back(&Output);
			}
			std::ranges::sort(Candidates, {}, [](const auto* Output) {
				return Output->AssetPath.GetView();
			});
			for (const Asset::Import::FImportRecordOutput* Candidate : Candidates)
			{
				const std::string_view Expected = Cast<DSkeletalMesh>(Asset)
					? Cast<DSkeletalMesh>(Asset)->GetSkeletonCompatibilityIdentity()
					: Cast<DAnimationClip>(Asset)->GetSkeletonCompatibilityIdentity();
				const Asset::FAssetCatalogEntry Data =
					Asset::FindAssetExact(Candidate->AssetPath);
				Asset::FAssetPackageInspection Package;
				std::string Actual;
				const Asset::FAssetPackageField* Field = Data
					&& Asset::InspectAssetPackage(Data->PhysicalPath, Package)
					? Package.FindField("SkeletonCompatibilityIdentity") : nullptr;
				if (!Expected.empty() && Field && Field->TryReadString(Actual)
					&& Expected == Actual)
					OutPaths.push_back(Candidate->AssetPath.ToString());
			}
			if (OutPaths.empty()) return nullptr;
			FAssetPath FirstPath;
			DObject* FirstCompatible = nullptr;
			return FAssetPath::TryCreate(OutPaths.front(), FirstPath)
				&& Asset::LoadAsset(FirstPath, FirstCompatible)
				? FirstCompatible : nullptr;
		}
	}

	MSkeletalAssetInspector::MSkeletalAssetInspector(::Durin::Editor::FWorkspaceManager& InWorkspaceManager)
		: WorkspaceManager(InWorkspaceManager) {}

	MSkeletalAssetInspector::~MSkeletalAssetInspector() = default;

	auto MSkeletalAssetInspector::GetWorkspaceType() const -> const ::Durin::Editor::FWorkspaceTypeId&
	{
		return Workspace::Type;
	}

	auto MSkeletalAssetInspector::OpenDocument(const ::Durin::Editor::FDocumentTab& Document)
		-> ::Durin::Editor::EDocumentOpenResult
	{
		if (Document.ResourceId.empty() || Document.DocumentKey.empty())
			return ::Durin::Editor::EDocumentOpenResult::Rejected;
		if (FindState(Document.DocumentKey)) return ::Durin::Editor::EDocumentOpenResult::Opened;
		FAssetPath AssetPath;
		std::string PathError;
		if (!FAssetPath::TryCreate(Document.ResourceId, AssetPath, &PathError))
		{
			ErrorMessage = std::move(PathError);
			return ::Durin::Editor::EDocumentOpenResult::Rejected;
		}
		::Durin::Editor::FWorkspaceAssetOpenCompatibility CompatibilityPolicy(AssetPath);
		DObject* Asset = nullptr;
		Asset::FAssetLoadReport Report;
		const Asset::FAssetResult Result = Asset::LoadAsset(AssetPath, Asset, &Report);
		if (!Result || !Asset || (Asset->GetClass() != DSkeleton::StaticClass()
			&& Asset->GetClass() != DSkeletalMesh::StaticClass()
			&& Asset->GetClass() != DAnimationClip::StaticClass()))
		{
			ErrorMessage = Result ? "The selected asset is not an exact skeletal asset." : Result.Message;
			return ::Durin::Editor::EDocumentOpenResult::Rejected;
		}
		std::string CompatibilityDiagnostic;
		if (CompatibilityPolicy.RejectIfIncompatible(Report, CompatibilityDiagnostic))
		{
			ErrorMessage = std::move(CompatibilityDiagnostic);
			return ::Durin::Editor::EDocumentOpenResult::Rejected;
		}
		FDocumentState State{.Asset = Asset};
		if (Cast<DSkeletalMesh>(Asset) || Cast<DAnimationClip>(Asset))
			State.PreviewPeer = ResolveSameRecordPreviewPeer(
				AssetPath, Asset, State.PreviewPeerPaths);
		Documents.emplace(Document.DocumentKey, std::move(State));
		return ::Durin::Editor::EDocumentOpenResult::Opened;
	}

	auto MSkeletalAssetInspector::ActivateDocument(const ::Durin::Editor::FDocumentTab& Document) -> void
	{
		DocumentHost.RequestFocus(Document.Id);
	}

	auto MSkeletalAssetInspector::RequestCloseDocument(const ::Durin::Editor::FDocumentTab& Document)
		-> ::Durin::Editor::EDocumentCloseResult
	{
		if (FDocumentState* State = FindState(Document.DocumentKey); State && State->Preview)
			State->Preview->SetVisible(false);
		Documents.erase(Document.DocumentKey);
		return ::Durin::Editor::EDocumentCloseResult::Closed;
	}

	auto MSkeletalAssetInspector::DrawWorkspace(bool bActive) -> bool
	{
		if (!bActive) for (auto& [Key, State] : Documents)
			if (State.Preview) State.Preview->SetVisible(false);
		return DocumentHost.DrawDocuments(
			WorkspaceManager, Workspace::Type,
			Workspace::RootKey,
			[this](const ::Durin::Editor::FDocumentTab& Document) { return FindState(Document.DocumentKey) != nullptr; },
			[this](const ::Durin::Editor::FDocumentTab& Document) {
				if (FDocumentState* State = FindState(Document.DocumentKey)) DrawDocument(Document, *State);
			});
	}

	auto MSkeletalAssetInspector::ResetLayout() -> void {}

	auto MSkeletalAssetInspector::FindState(std::string_view DocumentKey) -> FDocumentState*
	{
		const auto It = Documents.find(std::string(DocumentKey));
		return It == Documents.end() ? nullptr : &It->second;
	}

	auto MSkeletalAssetInspector::DrawDocument(
		const ::Durin::Editor::FDocumentTab& Document, FDocumentState& State) -> void
	{
		DObject* Asset = State.Asset.Get();
		if (!Asset)
		{
			ImGui::TextWrapped("Skeletal asset unavailable: the package was unloaded or replaced.");
			return;
		}
		ImGui::TextDisabled("Read-only");
		ImGui::SameLine();
		ImGui::TextUnformatted(Document.ResourceId.c_str());
		DSkeletalMesh* PreviewMesh = Cast<DSkeletalMesh>(Asset);
		DAnimationClip* PreviewClip = Cast<DAnimationClip>(Asset);
		if (!PreviewMesh) PreviewMesh = Cast<DSkeletalMesh>(State.PreviewPeer.Get());
		if (!PreviewClip) PreviewClip = Cast<DAnimationClip>(State.PreviewPeer.Get());
		if (PreviewMesh || PreviewClip)
		{
			if (!State.Preview) State.Preview = std::make_unique<FSkeletalAssetPreview>(NextPreviewId++);
			if (!State.PreviewPeerPaths.empty())
			{
				const char* Label = Cast<DSkeletalMesh>(Asset) ? "Preview clip" : "Preview mesh";
				const std::string& Current = State.PreviewPeerPaths[static_cast<size_t>(State.SelectedPreviewPeer)];
				ImGui::SetNextItemWidth(MonaImGui::ScaleUI(300.0f));
				if (ImGui::BeginCombo(Label, Current.c_str()))
				{
					for (int32 Index = 0; Index < static_cast<int32>(State.PreviewPeerPaths.size()); ++Index)
						if (ImGui::Selectable(State.PreviewPeerPaths[static_cast<size_t>(Index)].c_str(),
							State.SelectedPreviewPeer == Index))
						{
							FAssetPath PeerPath; DObject* Peer = nullptr;
							if (FAssetPath::TryCreate(State.PreviewPeerPaths[static_cast<size_t>(Index)], PeerPath)
								&& Asset::LoadAsset(PeerPath, Peer))
							{
								State.SelectedPreviewPeer = Index;
								State.PreviewPeer = Peer;
							}
						}
					ImGui::EndCombo();
				}
				PreviewMesh = Cast<DSkeletalMesh>(Asset)
					? Cast<DSkeletalMesh>(Asset) : Cast<DSkeletalMesh>(State.PreviewPeer.Get());
				PreviewClip = Cast<DAnimationClip>(Asset)
					? Cast<DAnimationClip>(Asset) : Cast<DAnimationClip>(State.PreviewPeer.Get());
			}
			if (ImGui::Button("Frame Selection")) State.Preview->ResetView();
			ImGui::SameLine();
			bool bWireframe = State.Preview->IsWireframe();
			if (ImGui::Checkbox("Wireframe", &bWireframe)) State.Preview->SetWireframe(bWireframe);
			ImGui::SameLine();
			bool bLit = State.Preview->IsLit();
			if (ImGui::Checkbox("Lit", &bLit)) State.Preview->SetLit(bLit);
			ImGui::SameLine();
			if (ImGui::Button(State.Preview->IsPlaying() ? "Pause" : "Play"))
			{
				if (State.Preview->IsPlaying()) State.Preview->Pause();
				else if (!State.Preview->Play(ErrorMessage)) {}
			}
			ImGui::SameLine();
			if (ImGui::Button("Reset")) State.Preview->ResetPlayback(ErrorMessage);
			ImGui::SameLine();
			bool bLooping = State.Preview->IsLooping();
			if (ImGui::Checkbox("Loop", &bLooping)) State.Preview->SetLooping(bLooping);
			ImGui::SameLine();
			float Rate = State.Preview->GetPlayRate();
			ImGui::SetNextItemWidth(100.0f);
			if (ImGui::SliderFloat("Rate", &Rate, 0.1f, 4.0f, "%.2fx"))
				State.Preview->SetPlayRate(Rate, ErrorMessage);
			if (PreviewClip)
			{
				float Time = State.Preview->GetPlaybackTime();
				const float Duration = PreviewClip->GetSummary().DurationSeconds;
				if (ImGui::SliderFloat("Timeline", &Time, 0.0f, std::max(0.0f, Duration), "%.3f s"))
					State.Preview->Seek(Time, ErrorMessage);
			}
			State.Preview->Draw(PreviewMesh, PreviewClip, MonaImGui::ScaleUI(360.0f));
		}
		ImGui::SeparatorText("Asset");
		if (ImGui::BeginTable("SkeletalAssetSummary", 2, ImGuiTableFlags_SizingStretchProp))
		{
			DrawInfoRow("Class", Asset->GetClass()->GetQualifiedName().ToString());
			if (const auto* Skeleton = Cast<DSkeleton>(Asset))
			{
				DrawInfoRow("Bones", std::to_string(Skeleton->GetBoneCount()));
				DrawInfoRow("Compatibility", Skeleton->GetCompatibilityIdentity());
			}
			else if (const auto* Mesh = Cast<DSkeletalMesh>(Asset))
			{
				const FSkeletalMeshSummary& Summary = Mesh->GetSummary();
				DrawInfoRow("Skeleton", Mesh->GetSkeleton() ? Mesh->GetSkeleton()->GetObjectPath() : "Unavailable");
				DrawInfoRow("Compatibility", Mesh->GetSkeletonCompatibilityIdentity());
				DrawInfoRow("Vertices", std::to_string(Summary.VertexCount));
				DrawInfoRow("Indices", std::to_string(Summary.IndexCount));
				DrawInfoRow("Sections", std::to_string(Summary.SectionCount));
				DrawInfoRow("Material slots", std::to_string(Mesh->GetNumMaterialSlots()));
				const auto Payload = Mesh->GetPayloadData();
				DrawInfoRow("Palette bones", std::to_string(Payload ? Payload->PaletteBoneIndices.size() : 0));
				DrawInfoRow("Storage", Mesh->WasLoadedFromDerivedDataCache() ? "Derived data cache" : "Authored/cooked payload");
			}
			else if (const auto* Clip = Cast<DAnimationClip>(Asset))
			{
				const FAnimationClipSummary& Summary = Clip->GetSummary();
				DrawInfoRow("Skeleton", Clip->GetSkeleton() ? Clip->GetSkeleton()->GetObjectPath() : "Unavailable");
				DrawInfoRow("Compatibility", Clip->GetSkeletonCompatibilityIdentity());
				DrawInfoRow("Duration", std::format("{:.3f} s", Summary.DurationSeconds));
				DrawInfoRow("Tracks", std::to_string(Summary.TrackCount));
				DrawInfoRow("Keys", std::to_string(Summary.KeyCount));
				DrawInfoRow("Storage", Clip->WasLoadedFromDerivedDataCache() ? "Derived data cache" : "Authored/cooked payload");
			}
			ImGui::EndTable();
		}

		if (const auto* Skeleton = Cast<DSkeleton>(Asset))
		{
			ImGui::SeparatorText("Canonical hierarchy");
			if (ImGui::BeginTable("SkeletonBones", 4,
				ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
				ImVec2(0.0f, 0.0f)))
			{
				ImGui::TableSetupColumn("Index"); ImGui::TableSetupColumn("Name");
				ImGui::TableSetupColumn("Parent"); ImGui::TableSetupColumn("Reference translation");
				ImGui::TableHeadersRow();
				const auto Bones = Skeleton->GetBones();
				ImGuiListClipper Clipper;
				Clipper.Begin(static_cast<int>(Bones.size()));
				while (Clipper.Step()) for (int Index = Clipper.DisplayStart; Index < Clipper.DisplayEnd; ++Index)
				{
					const FSkeletonBone& Bone = Bones[static_cast<size_t>(Index)];
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0); ImGui::Text("%d", Index);
					ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(Bone.Name.ToString().c_str());
					ImGui::TableSetColumnIndex(2); ImGui::Text("%d", Bone.ParentIndex);
					ImGui::TableSetColumnIndex(3); ImGui::Text("%.3f, %.3f, %.3f",
						Bone.ReferenceTransform.Row3.x, Bone.ReferenceTransform.Row3.y,
						Bone.ReferenceTransform.Row3.z);
				}
				ImGui::EndTable();
			}
		}
		else if (const auto* Clip = Cast<DAnimationClip>(Asset))
		{
			ImGui::SeparatorText("Tracks");
			const auto Payload = Clip->GetPayloadData();
			if (!Payload) ImGui::TextDisabled("Track payload unavailable");
			else if (ImGui::BeginTable("AnimationTracks", 5,
				ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
				ImVec2(0.0f, 0.0f)))
			{
				ImGui::TableSetupColumn("Track"); ImGui::TableSetupColumn("Bone");
				ImGui::TableSetupColumn("Path"); ImGui::TableSetupColumn("Interpolation");
				ImGui::TableSetupColumn("Keys"); ImGui::TableHeadersRow();
				ImGuiListClipper Clipper;
				Clipper.Begin(static_cast<int>(Payload->Tracks.size()));
				while (Clipper.Step()) for (int Index = Clipper.DisplayStart; Index < Clipper.DisplayEnd; ++Index)
				{
					const FAnimationTrackData& Track = Payload->Tracks[static_cast<size_t>(Index)];
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0); ImGui::Text("%d", Index);
					ImGui::TableSetColumnIndex(1); ImGui::Text("%u", Track.BoneIndex);
					ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(TrackPathLabel(Track.Path));
					ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(
						Track.Interpolation == EAnimationInterpolation::Step ? "Step" : "Linear");
					ImGui::TableSetColumnIndex(4); ImGui::Text("%zu", Track.Times.size());
				}
				ImGui::EndTable();
			}
		}
		if (!ErrorMessage.empty()) ImGui::TextWrapped("%s", ErrorMessage.c_str());
	}
}
