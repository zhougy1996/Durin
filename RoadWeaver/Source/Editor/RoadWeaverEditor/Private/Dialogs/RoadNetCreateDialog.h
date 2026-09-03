#pragma once

#include "Editor/Import/ImportDialogSupport.h"

namespace Durin::RoadNet::Editor
{
	// Routes a successful generated asset back to its Content Browser host.
	struct FCreateDialogCallbacks
	{
		std::function<void()> NotifyMountedContentChanged;
		std::function<bool(std::string_view)> RevealAsset;
		std::function<void(std::string)> ReportError;
	};

	// Owns the modal state and parameters for generating one straight bidirectional road.
	class FRoadNetCreateDialog
	{
	public:
		auto Open(
			std::string_view VirtualDirectory, FCreateDialogCallbacks InCallbacks) -> void;
		auto Draw(bool bAllowAssetMutation) -> void;

	private:
		auto CreateAsset() -> bool;
		auto BrowseDestination() -> void;
		auto SetError(std::string Message) -> void;

		::Durin::Editor::FImportDialogModalState ModalState;
		::Durin::Editor::FImportDialogDestinationModel Destination;
		FCreateDialogCallbacks Callbacks;
		std::string SubmissionError;
		double RoadLengthMeters = 100.0;
		int32 LanesPerDirection = 1;
		double LaneWidthMeters = 3.5;
		double SpeedLimitKilometersPerHour = 50.0;
	};
} // namespace Durin::RoadNet::Editor
