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
		auto GetSelection() const -> const std::unordered_set<FGuid>&
		{
			return SelectedNodes;
		}

	private:
		struct FVisualNode;
		auto FrameNodes(const FMaterialGraphView& View,
			const ImVec2& CanvasSize) -> void;

		ImVec2 Pan{40.0f, 40.0f};
		float Zoom = 1.0f;
		std::unordered_set<FGuid> SelectedNodes;
		FGuid PendingFrameNode;
		FGuid LinkSourceNode;
		ImVec2 MarqueeStart{};
		bool bMarqueeActive = false;
		std::unordered_map<FGuid, FMaterialGraphNodePresentation> DragStartPositions;
		ImVec2 DragStartMouse{};
		FMaterialGraphMoveSession MoveSession;
	};
}
