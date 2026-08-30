#pragma once

#include "MaterialGraphOperations.h"
#include "MonaImGui.h"

#include <variant>

namespace Durin
{
	class DTransactor;
}

namespace Durin::Editor::Material
{
	// Owns one document's transient material graph viewport and interaction state.
	class FMaterialGraphCanvas
	{
	public:
		using FReportError = std::function<void(std::string)>;
		FMaterialGraphCanvas();
		~FMaterialGraphCanvas();

		auto Draw(
			DMaterial& Material,
			::Durin::DTransactor& Transactions,
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
		auto GetViewport() const -> std::pair<float, ImVec2> { return {Zoom, Pan}; }
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
		struct FVisualGraph;

		struct FIdleInteraction {};
		struct FMovingInteraction
		{
			ImVec2 StartMouse{};
			std::unordered_map<FGuid, FMaterialGraphNodePresentation> StartPositions;
			std::optional<ImVec2> MaterialOutputStart;
		};
		struct FLinkingInteraction { FGuid SourceNode; };
		struct FReconnectingInputInteraction
		{
			FGuid DestinationNode;
			uint32 DestinationInputIndex = 0;
		};
		struct FReconnectingSurfaceInteraction
		{
			EMaterialSurfaceOutput Output = EMaterialSurfaceOutput::BaseColor;
		};
		struct FMarqueeInteraction { ImVec2 Start{}; };
		struct FInlineEditingInteraction
		{
			FGuid Node;
			std::array<float, 4> ConstantDraft{};
			std::array<int, 4> SwizzleDraft{};
		};
		struct FPaletteInteraction
		{
			FGuid SourceNode;
			ImVec2 GraphPosition{};
			bool bOpenRequested = true;
			int32 Selection = 0;
			std::array<char, 96> Search{};
		};
		struct FContextMenuInteraction
		{
			FGuid SourceNode;
			FGuid ContextNode;
			std::optional<EMaterialSurfaceOutput> SurfaceOutput;
			ImVec2 GraphPosition{};
		};
		using FInteraction = std::variant<
			FIdleInteraction,
			FMovingInteraction,
			FLinkingInteraction,
			FReconnectingInputInteraction,
			FReconnectingSurfaceInteraction,
			FMarqueeInteraction,
			FInlineEditingInteraction,
			FPaletteInteraction,
			FContextMenuInteraction>;

		auto PrepareView(DMaterial& Material) -> const FMaterialGraphView&;
		auto PrepareVisualGraph(const FMaterialGraphView& View,
			const ImVec2& CanvasMinimum) -> const FVisualGraph&;
		auto FrameNodes(const FMaterialGraphView& View,
			const ImVec2& CanvasSize) -> void;
		auto DrawLinks(const FVisualGraph& VisualGraph,
			const ImVec2& CanvasMinimum, const ImVec2& CanvasMaximum,
			ImDrawList& DrawList) const -> void;
		auto HandleKeyboardInput(DMaterial& Material,
			::Durin::DTransactor& Transactions, const FMaterialGraphView& View,
			const ImVec2& CanvasMinimum, const ImVec2& CanvasSize,
			const ImVec2& Mouse, bool bInputAvailable,
			const FReportError& ReportError) -> void;
		auto CopyNodes(DMaterial& Material, std::span<const FGuid> NodeIds,
			const FReportError& ReportError) -> void;
		auto CutNodes(DMaterial& Material, ::Durin::DTransactor& Transactions,
			std::span<const FGuid> NodeIds,
			const FReportError& ReportError) -> void;
		auto DuplicateNodes(DMaterial& Material,
			::Durin::DTransactor& Transactions, std::span<const FGuid> NodeIds,
			const FReportError& ReportError) -> void;
		auto PasteNodes(DMaterial& Material, ::Durin::DTransactor& Transactions,
			const ImVec2& GraphPosition,
			const FReportError& ReportError) -> void;
		auto RemoveNodes(DMaterial& Material, ::Durin::DTransactor& Transactions,
			std::span<const FGuid> NodeIds,
			const FReportError& ReportError) -> void;
		auto DrawContextMenu(DMaterial& Material,
			::Durin::DTransactor& Transactions, const FMaterialGraphView& View,
			const FReportError& ReportError) -> void;
		auto DrawPalette(DMaterial& Material,
			::Durin::DTransactor& Transactions, const FMaterialGraphView& View,
			const FReportError& ReportError) -> void;
		auto ResetInteraction() -> void;

		ImVec2 Pan{40.0f, 40.0f};
		float Zoom = 1.0f;
		EMaterialGraphDetailLevel DetailLevel = EMaterialGraphDetailLevel::Editing;
		std::optional<ImVec2> SurfaceGraphPosition;
		std::unordered_set<FGuid> SelectedNodes;
		FGuid PendingFrameNode;
		std::optional<EMaterialSurfaceOutput> SelectedSurfaceOutput;
		bool bMaterialOutputSelected = false;
		bool bPendingFrameSurface = false;
		std::vector<std::string> RecentPaletteEntries;
		std::unordered_set<std::string> FavoritePaletteEntries;
		DMaterial* CachedMaterial = nullptr;
		uint64 CatalogSchemaRevision = 0;
		uint64 CatalogRevision = 0;
		uint64 FavoritePaletteRevision = 0;
		uint64 RecentPaletteRevision = 0;
		uint64 CachedPaletteCatalogRevision = 0;
		uint64 CachedFavoritePaletteRevision = 0;
		uint64 CachedRecentPaletteRevision = 0;
		std::string CachedPaletteQuery;
		std::optional<EMaterialProgramValueType> CachedPaletteSourceType;
		std::vector<size_t> CachedPaletteResults;
		uint64 CachedProgramRevision = 0;
		uint64 CachedPresentationRevision = 0;
		uint64 CachedSchemaRevision = 0;
		std::vector<FMaterialGraphCatalogEntry> Catalog;
		FMaterialGraphView CachedView;
		std::unique_ptr<FVisualGraph> CachedVisualGraph;
		bool bVisualGraphTopologyStale = true;
		std::array<std::array<float, 4>, 8> SurfaceDefaultDrafts{};
		std::array<bool, 8> bSurfaceDefaultDraftInitialized{};
		FInteraction Interaction = FIdleInteraction{};
		FMaterialGraphMoveSession MoveSession;
		FMaterialGraphParameterEditSession ParameterEditSession;
	};
}
