#pragma once

#include "MaterialGraphOperations.h"
#include "MaterialGraphReadModel.h"
#include "MaterialGraphDocument.h"
#include "MonaImGui.h"

#include <variant>

namespace Durin
{
	class DTransactor;
	class DMaterialFunction;
}

namespace Durin::Editor::Material
{
	// All canvas selections refer to authored expression identities.
	using FMaterialGraphCanvasNodeId = std::variant<FGuid>;

	// Owns one document's transient material graph viewport and interaction state.
	class FMaterialGraphCanvas
	{
	public:
		using FReportError = std::function<void(std::string)>;
		// One immutable document binding for the canvas lifetime; never rebound on tab activation.
		explicit FMaterialGraphCanvas(FMaterialGraphDocument Document);
		~FMaterialGraphCanvas();
		// Release shared clipboard object references before the object system shuts down.
		static auto ClearSharedClipboard() -> void;

		auto Draw(
			::Durin::DTransactor& Transactions,
			float Height,
			const FReportError& ReportError,
			const std::function<void(std::string_view)>& OpenFunction = {}) -> void;
		auto SelectAndFrame(const FGuid& NodeId) -> bool;
		auto DrawSelectionDetails(DTransactor& Transactions,
			const FReportError& ReportError) -> void;
		auto DrawParameterValue( const FMaterialParameterDefinition& Parameter,
			DTransactor& Transactions, const FReportError& ReportError) -> void;
		auto EndParameterFrame() -> void;
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
		const FMaterialGraphDocument GraphDocument;
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

		auto HandleViewportInput(const ImVec2& Minimum, const ImVec2& Mouse, bool bHovered) -> void;
		auto HitTest(const FVisualGraph& VisualGraph, const ImVec2& Minimum,
			const ImVec2& Maximum, const ImVec2& Mouse) const -> FPointerHit;
		auto HandlePointerInput(DObject& Owner, DTransactor& Transactions,
			const FMaterialGraphView& View, const FVisualGraph& VisualGraph,
			const ImVec2& Minimum, const ImVec2& Maximum, const ImVec2& Size,
			const ImVec2& Mouse, bool bPointerAvailable,
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
		};
		struct FLinkingInteraction { FGuid SourceNode; uint8 SourceOutputIndex = 0; FGuid SourceOutputId; };
		struct FReconnectingInputInteraction
		{
			FMaterialGraphPinAddress Destination;
		};

		struct FMarqueeInteraction { ImVec2 Start{}; };
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
			std::vector<std::string> FunctionPaths;
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
			FMarqueeInteraction,
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
		auto RememberCreation(const FMaterialGraphCreationAction& Node) -> void;
		auto HasClipboard() const -> bool;
		auto DrawCreationMenu(DObject& Owner,
			::Durin::DTransactor& Transactions, const FMaterialGraphView& View,
			const FReportError& ReportError) -> void;
		auto ResetInteraction() -> void;
		auto PrepareFunctionView(DMaterialFunction& Function) -> void;
		auto PrepareDocumentView(DObject& Owner) -> void;
		auto PrepareDetailsView(DObject& Owner) -> const FMaterialGraphView&;
		auto HandleCreationShortcut(DObject& Owner, DTransactor& Transactions,
			const ImVec2& Position, const FReportError& ReportError) -> bool;
		auto DrawNodeHeading(const FVisualNode& Visual, ImDrawList& DrawList,
			const DMaterial* Material = nullptr) const -> void;

		ImVec2 Pan{40.0f, 40.0f};
		float Zoom = 1.0f;
		bool bFunctionGraph = false;
		FGuid OutputNodeId;
		EMaterialGraphDetailLevel DetailLevel = EMaterialGraphDetailLevel::Editing;
		std::unordered_set<FMaterialGraphCanvasNodeId> SelectedNodes;
		FGuid PendingFrameNode;
		std::optional<FMaterialProgramDiagnostic> SelectedDiagnostic;
		std::optional<EMaterialSurfaceOutput> SelectedSurfaceOutput;
		std::vector<std::string> RecentCreationMenuEntries;
		FMaterialGraphReadModel ReadModel;
		bool bViewStale = true;
		std::optional<ImVec2> LastPasteAnchor;
		uint32 RepeatedPasteCount = 0;
		bool bCreationMenuResultsDirty = true;
		std::string CachedCreationMenuQuery;
		std::optional<EMaterialProgramValueType> CachedCreationMenuSourceType;
		std::vector<FMaterialGraphCreationAction> CachedCreationActions;
		std::vector<size_t> CachedCreationMenuResults;
		size_t CachedCreationMenuRecentCount = 0;
		std::vector<FMaterialGraphCatalogEntry> Catalog;
		FMaterialGraphView CachedView;
		std::unordered_map<FGuid, size_t> CachedNodeIndices;
		std::unique_ptr<FVisualGraph> CachedVisualGraph;
		std::shared_ptr<FTexturePreviewState> TexturePreviews;
		bool bVisualGraphTopologyStale = true;
		bool bShowAdvancedInputs = false;
		std::array<char, 129> PromotionNameDraft{};
		std::array<char, 256> NodeTextureSearch{};
		FInteraction Interaction = FIdleInteraction{};
		FMaterialGraphMoveSession MoveSession;
		FMaterialGraphParameterEditSession ParameterSession;
		FGuid EditingParameterId;
		ImGuiID ParameterWidget = 0;
		int ParameterFrame = -1;
	};
}
