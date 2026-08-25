#pragma once

#include "MaterialGraphAuthoring.h"
#include "MonaImGui.h"

namespace Durin::Editor
{
	class FTransactionManager;
}

namespace Durin::Editor::Material
{
	// Owns one document's transient material graph viewport and interaction state.
	class FMaterialGraphCanvas
	{
	public:
		using FReportError = std::function<void(std::string)>;

		auto Draw(
			DMaterial& Material,
			FTransactionManager& Transactions,
			float Height,
			const FReportError& ReportError) -> void;
		auto SelectAndFrame(const FGuid& NodeId) -> bool;
		auto SelectAndFrameDiagnostic(
			const FMaterialProgramDiagnostic& Diagnostic) -> bool;
		auto CancelInteraction() -> void;
		// Sets detached viewport state for editor automation and rendered qualification.
		auto SetViewport(float InZoom, const ImVec2& InPan) -> void
		{
			Zoom = std::clamp(InZoom, 0.25f, 2.0f);
			Pan = InPan;
			DetailLevel = FMaterialGraphGeometry::SelectDetailLevel(
				Zoom, EMaterialGraphDetailLevel::Readable);
		}
		auto GetSelection() const -> const std::unordered_set<FGuid>&
		{
			return SelectedNodes;
		}
		auto GetSelectedSurfaceOutput() const
			-> std::optional<EMaterialSurfaceOutput>
		{
			return SelectedSurfaceOutput;
		}

	private:
		struct FVisualNode;
		auto FrameNodes(const FMaterialGraphView& View,
			const ImVec2& CanvasSize) -> void;

		ImVec2 Pan{40.0f, 40.0f};
		float Zoom = 1.0f;
		EMaterialGraphDetailLevel DetailLevel = EMaterialGraphDetailLevel::Editing;
		std::unordered_set<FGuid> SelectedNodes;
		FGuid PendingFrameNode;
		std::optional<EMaterialSurfaceOutput> SelectedSurfaceOutput;
		bool bPendingFrameSurface = false;
		FGuid LinkSourceNode;
		FGuid PaletteSourceNode;
		ImVec2 PaletteGraphPosition{};
		std::optional<FMaterialGraphCreateNodeRequest> CreationDraft;
		std::vector<std::vector<EMaterialProgramValueType>> CreationDraftAcceptedTypes;
		uint32 CreationDraftInputIndex = 0;
		std::array<char, 96> PaletteSearch{};
		FGuid ReconnectDestinationNode;
		uint32 ReconnectDestinationInputIndex = 0;
		FGuid InlineEditNode;
		std::array<float, 4> InlineConstantDraft{};
		std::array<int, 4> InlineSwizzleDraft{};
		std::array<std::array<float, 4>, 8> SurfaceDefaultDrafts{};
		std::array<bool, 8> bSurfaceDefaultDraftInitialized{};
		ImVec2 MarqueeStart{};
		bool bMarqueeActive = false;
		std::unordered_map<FGuid, FMaterialGraphNodePresentation> DragStartPositions;
		ImVec2 DragStartMouse{};
		FMaterialGraphMoveSession MoveSession;
	};
}
