#pragma once

#include "MaterialGraphOperations.h"
#include "MonaImGui.h"

#include <variant>

namespace Durin
{
	class DTransactor;
	class DMaterialFunction;
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
		auto DrawSelectionDetails(DObject& Owner, DTransactor& Transactions,
			const FReportError& ReportError) -> void;
		auto DrawFunction(DMaterialFunction& Function, ::Durin::DTransactor& Transactions,
			float Height, const FReportError& ReportError,
			const std::function<void(std::string_view)>& OpenFunction) -> void;
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
		struct FPointerHit
		{
			const FVisualNode* Node = nullptr;
			const FVisualNode* InputNode = nullptr;
			const FVisualNode* OutputNode = nullptr;
			uint32 InputIndex = 0;
			size_t OutputIndex = 0;
		};
		struct FSurfaceInteractionTarget
		{
			std::optional<EMaterialSurfaceOutput> Output;
			bool bHoveredHeader = false;
			bool bHoveredBody = false;
			ImVec2 GraphPosition{};
			std::array<ImVec2, 9> Pins{};
		};
		auto HandleViewportInput(const ImVec2& Minimum, const ImVec2& Mouse, bool bHovered) -> void;
		auto HitTest(const FVisualGraph& VisualGraph, const ImVec2& Minimum,
			const ImVec2& Maximum, const ImVec2& Mouse) const -> FPointerHit;
		auto HandlePointerInput(DObject& Owner, DTransactor& Transactions,
			const FMaterialGraphView& View, const FVisualGraph& VisualGraph,
			const ImVec2& Minimum, const ImVec2& Maximum, const ImVec2& Size,
			const ImVec2& Mouse, bool bPointerAvailable, const FSurfaceInteractionTarget& Surface,
			const FReportError& ReportError) -> void;
		struct FTexturePreviewState;
		auto UpdateTexturePreviews(DMaterial& Material) -> void;
		auto DrawTexturePreview(const FGuid& NodeId, const ImVec2& Position, float Size) -> void;
		auto AcceptTextureDrop(DMaterial& Material, DTransactor& Transactions,
			const ImVec2& CanvasMinimum, const FReportError& ReportError) -> void;

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
			FGuid DestinationInputId;
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
		auto HandleKeyboardInput(DObject& Owner,
			::Durin::DTransactor& Transactions, const FMaterialGraphView& View,
			const ImVec2& CanvasMinimum, const ImVec2& CanvasSize,
			const ImVec2& Mouse, bool bInputAvailable,
			const FReportError& ReportError) -> void;
		auto CopyNodes(DObject& Owner, std::span<const FGuid> NodeIds,
			const FReportError& ReportError) -> void;
		auto CutNodes(DObject& Owner, ::Durin::DTransactor& Transactions,
			std::span<const FGuid> NodeIds,
			const FReportError& ReportError) -> void;
		auto DuplicateNodes(DObject& Owner,
			::Durin::DTransactor& Transactions, std::span<const FGuid> NodeIds,
			const FReportError& ReportError) -> void;
		auto PasteNodes(DObject& Owner, ::Durin::DTransactor& Transactions,
			const ImVec2& GraphPosition,
			const FReportError& ReportError) -> void;
		auto RemoveNodes(DObject& Owner, ::Durin::DTransactor& Transactions,
			std::span<const FGuid> NodeIds,
			const FReportError& ReportError) -> void;
		auto DrawContextMenu(DObject& Owner,
			::Durin::DTransactor& Transactions, const FMaterialGraphView& View,
			const FReportError& ReportError) -> void;
		auto RememberCreation(const FMaterialGraphCatalogEntry& Node) -> void;
		auto HasClipboard() const -> bool;
		auto DrawCreationMenu(DObject& Owner,
			::Durin::DTransactor& Transactions, const FMaterialGraphView& View,
			const FReportError& ReportError) -> void;
		auto ResetInteraction() -> void;
		auto PrepareFunctionView(DMaterialFunction& Function) -> void;
		auto PrepareDetailsView(DObject& Owner) -> const FMaterialGraphView&;
		auto HandleCreationShortcut(DObject& Owner, DTransactor& Transactions,
			const ImVec2& Position, const FReportError& ReportError) -> bool;
		auto DrawNodeHeading(const FVisualNode& Visual, ImDrawList& DrawList,
			const DMaterial* Material = nullptr) const -> void;

		ImVec2 Pan{40.0f, 40.0f};
		float Zoom = 1.0f;
		bool bFunctionGraph = false;
		EMaterialGraphDetailLevel DetailLevel = EMaterialGraphDetailLevel::Editing;
		std::optional<ImVec2> SurfaceGraphPosition;
		std::unordered_set<FMaterialGraphCanvasNodeId> SelectedNodes;
		FGuid PendingFrameNode;
		std::optional<FMaterialProgramDiagnostic> SelectedDiagnostic;
		std::optional<EMaterialSurfaceOutput> SelectedSurfaceOutput;
		bool bPendingFrameSurface = false;
		std::vector<std::string> RecentCreationMenuEntries;
		DMaterial* CachedMaterial = nullptr;
		DMaterialFunction* CachedFunction = nullptr;
		std::vector<std::pair<DObject*, uint64>> CachedFunctionRevisions;
		std::vector<FMaterialGraphNodePresentation> CachedFunctionPositions;
		std::optional<ImVec2> LastPasteAnchor;
		uint32 RepeatedPasteCount = 0;
		uint64 CatalogRevision = 0;
		uint64 RecentCreationMenuRevision = 0;
		uint64 CachedCreationMenuCatalogRevision = 0;
		uint64 CachedRecentCreationMenuRevision = 0;
		std::string CachedCreationMenuQuery;
		std::optional<EMaterialProgramValueType> CachedCreationMenuSourceType;
		std::vector<size_t> CachedCreationMenuResults;
		size_t CachedCreationMenuRecentCount = 0;
		uint64 CachedProgramRevision = 0;
		uint64 CachedExpressionRevision = 0;
		uint64 CachedRenderStateVersion = 0;
		uint64 CachedPresentationRevision = 0;
		uint64 CachedSchemaRevision = 0;
		std::vector<FMaterialGraphCatalogEntry> Catalog;
		FMaterialGraphView CachedView;
		// Full inspection shared by Details and the canvas, before pin visibility filtering.
		FMaterialGraphView CachedInspection;
		std::unordered_map<FGuid, size_t> CachedNodeIndices;
		std::unique_ptr<FVisualGraph> CachedVisualGraph;
		std::shared_ptr<FTexturePreviewState> TexturePreviews;
		bool bVisualGraphTopologyStale = true;
		bool bShowAdvancedInputs = false;
		std::array<char, 129> PromotionNameDraft{};
		std::array<char, 256> NodeTextureSearch{};
		FInteraction Interaction = FIdleInteraction{};
		FMaterialGraphMoveSession MoveSession;
		FMaterialGraphParameterEditSession ParameterEditSession;
	};
}
