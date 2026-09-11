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
	// Canvas identity includes the derived terminal without adding a semantic program node.
	enum class EMaterialGraphTerminal { MaterialOutput };
	using FMaterialGraphCanvasNodeId = std::variant<FGuid, EMaterialGraphTerminal>;

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
		auto GetSelection() const -> const std::unordered_set<FMaterialGraphCanvasNodeId>&
		{
			return SelectedNodes;
		}
		auto GetSelectedSurfaceOutput() const
			-> std::optional<EMaterialSurfaceOutput>
		{
			return SelectedSurfaceOutput;
		}

	private:
		friend struct FMaterialGraphCanvasTestAccess;
		struct FVisualNode;
		struct FVisualGraph;

		struct FIdleInteraction {};
		struct FMovingInteraction
		{
			ImVec2 StartMouse{};
			std::unordered_map<FGuid, FMaterialGraphNodePresentation> StartPositions;
			std::optional<ImVec2> MaterialOutputStart;
		};
		struct FLinkingInteraction { FGuid SourceNode; uint8 SourceOutputIndex = 0; FGuid SourceOutputId; };
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
		struct FNodeCreationMenuInteraction
		{
			FGuid SourceNode;
			uint8 SourceOutputIndex = 0;
			FGuid SourceOutputId;
			ImVec2 GraphPosition{};
			bool bOpenRequested = true;
			int32 Selection = 0;
			std::array<char, 96> Search{};
			std::optional<FMaterialProgramNode> PendingParameter;
			std::array<char, 96> ParameterFilter{};
		};
		struct FContextMenuInteraction
		{
			FGuid ContextNode;
			std::optional<EMaterialSurfaceOutput> SurfaceOutput;
		};
		using FInteraction = std::variant<
			FIdleInteraction,
			FMovingInteraction,
			FLinkingInteraction,
			FReconnectingInputInteraction,
			FReconnectingSurfaceInteraction,
			FMarqueeInteraction,
			FInlineEditingInteraction,
			FNodeCreationMenuInteraction,
			FContextMenuInteraction>;

		auto PrepareView(DMaterial& Material) -> const FMaterialGraphView&;
		auto PrepareVisualGraph(const FMaterialGraphView& View,
			const ImVec2& CanvasMinimum) -> const FVisualGraph&;
		enum class EFrameScope { All, Selection };
		auto GetSelectedProgramNodes() const -> std::vector<FGuid>;
		auto FrameNodes(const FMaterialGraphView& View,
			const ImVec2& CanvasSize, EFrameScope Scope) -> void;
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
		auto RememberCreation(const FMaterialProgramNode& Node) -> void;
		auto HasClipboard() const -> bool;
		auto DrawCreationMenu(DMaterial& Material,
			::Durin::DTransactor& Transactions, const FMaterialGraphView& View,
			const FReportError& ReportError) -> void;
		auto ResetInteraction() -> void;

		ImVec2 Pan{40.0f, 40.0f};
		float Zoom = 1.0f;
		EMaterialGraphDetailLevel DetailLevel = EMaterialGraphDetailLevel::Editing;
		std::optional<ImVec2> SurfaceGraphPosition;
		std::unordered_set<FMaterialGraphCanvasNodeId> SelectedNodes;
		FGuid PendingFrameNode;
		std::optional<EMaterialSurfaceOutput> SelectedSurfaceOutput;
		bool bPendingFrameSurface = false;
		std::vector<std::string> RecentCreationMenuEntries;
		std::unordered_set<std::string> FavoriteCreationMenuEntries;
		DMaterial* CachedMaterial = nullptr;
		uint64 CatalogRevision = 0;
		uint64 FavoriteCreationMenuRevision = 0;
		uint64 RecentCreationMenuRevision = 0;
		uint64 CachedCreationMenuCatalogRevision = 0;
		uint64 CachedFavoriteCreationMenuRevision = 0;
		uint64 CachedRecentCreationMenuRevision = 0;
		std::string CachedCreationMenuQuery;
		std::optional<EMaterialProgramValueType> CachedCreationMenuSourceType;
		std::vector<size_t> CachedCreationMenuResults;
		uint64 CachedProgramRevision = 0;
		uint64 CachedPresentationRevision = 0;
		uint64 CachedSchemaRevision = 0;
		std::vector<FMaterialGraphCatalogEntry> Catalog;
		FMaterialGraphView CachedView;
		std::unordered_map<FGuid, size_t> CachedNodeIndices;
		std::unique_ptr<FVisualGraph> CachedVisualGraph;
		bool bVisualGraphTopologyStale = true;
		std::array<std::array<float, 4>, 8> SurfaceDefaultDrafts{};
		std::array<bool, 8> bSurfaceDefaultDraftInitialized{};
		std::array<char, 129> PromotionNameDraft{};
		FInteraction Interaction = FIdleInteraction{};
		FMaterialGraphMoveSession MoveSession;
		FMaterialGraphParameterEditSession ParameterEditSession;
	};
}
