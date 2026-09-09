#include "Dialogs/RoadNetCreateDialog.h"

#include "Asset/PackageSerialization.h"
#include "AssetTools/IAssetTools.h"
#include "MonaImGui.h"
#include "RoadNet/RoadNet.h"
#include "RoadNet/RoadNetBuilder.h"
#include "RoadNet/RoadSurface.h"
#include "Math/Operations.h"

namespace Durin::RoadNet::Editor
{
	void FRoadNetCreateDialog::Open(
		std::string_view VirtualDirectory, FCreateDialogCallbacks InCallbacks)
	{
		Callbacks = std::move(InCallbacks);
		SubmissionError.clear();
		bSpherical = true;
		PlanetRadiusMeters = 1000.0;
		RoadLengthMeters = 100.0;
		LanesPerDirection = 1;
		LaneWidthMeters = 3.5;
		SpeedLimitKilometersPerHour = 50.0;
		Destination.Reset(VirtualDirectory);
		Destination.SuggestPath(
			Destination.MakeSuggestedPath("NewRoadNet", "/Game/"));
		ModalState.RequestOpen();
	}

	void FRoadNetCreateDialog::Draw(bool bAllowAssetMutation)
	{
		ModalState.OpenPopupIfRequested("Create Road Net");
		const MonaImGui::FUIStyleMetrics Metrics = MonaImGui::GetUIStyleMetrics();
		ImGui::SetNextWindowSize(
			ImVec2(Metrics.WidePopupWidth, 0.0f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal("Create Road Net", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize
				| ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
			return;

		ImGui::TextUnformatted(
			"Create a fixed planet binding and one final bidirectional road curve.");
		ImGui::Spacing();
		ImGui::SeparatorText("Planet and initial surface");
		ImGui::InputDouble("Planet radius (m)", &PlanetRadiusMeters, 10.0, 100.0, "%.2f");
		int SurfaceChoice = bSpherical ? 1 : 0;
		if (ImGui::Combo("Initial surface", &SurfaceChoice, "Plane\0Sphere\0")) bSpherical = SurfaceChoice == 1;
		ImGui::TextWrapped("Network origin is at the north pole; planet center is (0, 0, -radius). "
			"The radius is fixed after creation. Later curve edits may change elevation.");
		ImGui::SeparatorText("Road");
		ImGui::InputDouble("Length (m)", &RoadLengthMeters, 1.0, 10.0, "%.2f");
		ImGui::InputInt("Lanes per direction", &LanesPerDirection);
		ImGui::InputDouble("Lane width (m)", &LaneWidthMeters, 0.1, 0.5, "%.2f");
		ImGui::InputDouble(
			"Speed limit (km/h)", &SpeedLimitKilometersPerHour, 1.0, 10.0, "%.1f");

		ImGui::Spacing();
		ImGui::SeparatorText("Destination");
		const float BrowseButtonWidth = Metrics.StandardButtonWidth;
		if (Destination.DrawRow(
			"Asset path", "##RoadNetAssetPath", "/Game/Roads/NewRoadNet",
			"Choose...", BrowseButtonWidth))
			BrowseDestination();

		const ::Durin::Editor::FAssetDestinationValidation DestinationValidation =
			Destination.Inspect();
		const bool bParametersValid = std::isfinite(RoadLengthMeters)
			&& RoadLengthMeters > 0.0 && LanesPerDirection >= 1
			&& LanesPerDirection <= 16 && std::isfinite(LaneWidthMeters)
			&& LaneWidthMeters > 0.0 && std::isfinite(SpeedLimitKilometersPerHour)
			&& SpeedLimitKilometersPerHour > 0.0
			&& std::isfinite(PlanetRadiusMeters) && PlanetRadiusMeters >= 1.0 && PlanetRadiusMeters <= RoadCoordinateLimit
			&& (!bSpherical || RoadLengthMeters < Math::Pi<double>() * PlanetRadiusMeters);
		if (!DestinationValidation.Message.empty())
			::Durin::Editor::DrawImportDialogWarning(DestinationValidation.Message);
		else if (!bParametersValid)
			::Durin::Editor::DrawImportDialogWarning(
				"Use positive road dimensions and speed, 1 to 16 lanes, radius 1 to 10000000 m, and a spherical arc shorter than half a circumference.");
		::Durin::Editor::DrawImportDialogWarning(SubmissionError);

		ImGui::Spacing();
		ImGui::Separator();
		const float ButtonWidth = Metrics.StandardButtonWidth;
		const float ButtonsWidth = ButtonWidth * 2.0f + ImGui::GetStyle().ItemSpacing.x;
		ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
			ImGui::GetWindowContentRegionMax().x - ButtonsWidth));
		if (ImGui::Button("Cancel", ImVec2(ButtonWidth, 0.0f)))
			ImGui::CloseCurrentPopup();
		ImGui::SameLine();
		ImGui::BeginDisabled(
			!bAllowAssetMutation || !DestinationValidation || !bParametersValid);
		if (ImGui::Button("Create", ImVec2(ButtonWidth, 0.0f)) && CreateAsset())
			ImGui::CloseCurrentPopup();
		ImGui::EndDisabled();
		ImGui::EndPopup();
	}

	auto FRoadNetCreateDialog::CreateAsset() -> bool
	{
		SubmissionError.clear();
		const ::Durin::Editor::FAssetDestinationValidation DestinationValidation =
			Destination.Inspect();
		if (!DestinationValidation)
		{
			SetError(DestinationValidation.Message);
			return false;
		}
		const FPackagePath& PackagePath = DestinationValidation.AssetPath;
		FTopLevelAssetPath AssetPath;
		if (!FTopLevelAssetPath::TryCreate(
			PackagePath, PackagePath.GetPackageName(), AssetPath))
		{
			SetError("The Road Net top-level asset path is invalid.");
			return false;
		}
		const FAssetToolsResult Created = IAssetTools::Get().CreateAsset(
			AssetPath, DRoadNet::StaticClass());
		DRoadNet* RoadNet = Cast<DRoadNet>(Created.Asset);
		if (!Created || !RoadNet)
		{
			SetError(Created.Message.empty()
				? "Could not create the Road Net asset." : Created.Message);
			return false;
		}

		const double Angle = RoadLengthMeters / PlanetRadiusMeters;
		const FVector3 End = bSpherical
			? FVector3(PlanetRadiusMeters * std::sin(Angle), 0, PlanetRadiusMeters * (std::cos(Angle) - 1))
			: FVector3(RoadLengthMeters, 0, 0);
		FRoadNetBuilder Builder;
		const FGuid StartNodeId = Builder.AddNode(
			"Start", FVector3{0.0, 0.0, 0.0});
		const FGuid EndNodeId = Builder.AddNode(
			"End", End);
		FRoad Road;
		Road.Name = "Main Road";
		Road.StartNodeId = StartNodeId;
		Road.EndNodeId = EndNodeId;
		Road.SpeedLimitMetersPerSecond = SpeedLimitKilometersPerHour / 3.6;
		Road.ReferenceLine.SetPoints({FSplinePoint(FVector3{0.0, 0.0, 0.0}),
			FSplinePoint(End)});
		FLaneSection Section;
		Section.StartDistanceMeters = 0.0;
		Section.EndDistanceMeters = Road.ReferenceLine.BuildEvaluationData()->GetLocalLength();
		Section.Lanes.reserve(static_cast<size_t>(LanesPerDirection) * 2);
		for (int32 LaneIndex = 1; LaneIndex <= LanesPerDirection; ++LaneIndex)
		{
			Section.Lanes.push_back({
				.Index = LaneIndex,
				.Direction = ELaneDirection::AlongReferenceLine,
				.WidthMeters = LaneWidthMeters});
			Section.Lanes.push_back({
				.Index = -LaneIndex,
				.Direction = ELaneDirection::AgainstReferenceLine,
				.WidthMeters = LaneWidthMeters});
		}
		Road.LaneSections.push_back(std::move(Section));
		Builder.AddRoad(std::move(Road));
		std::string Error;
		auto Candidate = Builder.TakeDefinition();
		Candidate.Planet.Id = FGuid::NewGuid();
		Candidate.Planet.Center = {0, 0, -PlanetRadiusMeters};
		Candidate.Planet.RadiusMeters = PlanetRadiusMeters;
		FRoadSurface Operation;
		Operation.Mode = bSpherical ? ERoadSurfaceMode::Sphere : ERoadSurfaceMode::Plane;
		Operation.Origin = bSpherical ? Candidate.Planet.Center : FVector3(0);
		Operation.RadiusMeters = PlanetRadiusMeters;
		if (!FitRoadDefinition(Candidate, Operation, Error) || !RoadNet->SetDefinition(std::move(Candidate), Error))
		{
			IAssetTools::Get().DiscardPackage(RoadNet->GetPackage());
			SetError(Error);
			return false;
		}
		const FAssetResult Saved = SavePackage(RoadNet->GetPackage());
		if (!Saved)
		{
			UnloadPackage(PackagePath);
			SetError(Saved.Message.empty()
				? "The Road Net asset could not be saved." : Saved.Message);
			return false;
		}

		const std::string CreatedPath = PackagePath.ToString();
		if (Callbacks.NotifyMountedContentChanged)
			Callbacks.NotifyMountedContentChanged();
		if (Callbacks.RevealAsset) Callbacks.RevealAsset(CreatedPath);
		return true;
	}

	void FRoadNetCreateDialog::BrowseDestination()
	{
		::Durin::Editor::FImportDialogCallbacks DialogCallbacks{
			.ReportError = [this](std::string Message) {
				SetError(std::move(Message));
			}};
		Destination.Browse(
			"Choose a Road Net Asset Path", "NewRoadNet.dasset",
			"The selected Road Net asset path is too long.",
			"Road Net assets must be saved inside a package-enabled mount.",
			DialogCallbacks);
	}

	void FRoadNetCreateDialog::SetError(std::string Message)
	{
		SubmissionError = Message;
		if (Callbacks.ReportError) Callbacks.ReportError(std::move(Message));
	}
} // namespace Durin::RoadNet::Editor
