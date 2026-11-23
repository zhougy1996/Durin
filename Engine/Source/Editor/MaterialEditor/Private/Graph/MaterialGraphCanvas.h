#pragma once

#include "CoreMinimal.h"
#include "Graph/MaterialGraphInteraction.h"
#include "Graph/MaterialEditorPreviewParameter.h"

#include "MaterialGraphOperations.h"
#include "MaterialGraph.h"
#include "MonaImGui.h"
#include "PropertyEditor/DetailPanel.h"
#include "PropertyEditor/PropertyControlFrame.h"

namespace Durin
{
	class DTransactor;
	class DMaterialFunction;
	class DMaterialExpressionParameter;
}

namespace Durin::MaterialEditor
{
	struct FMaterialGraphEditorServices
	{
		std::function<void(std::string)> ReportError = [](std::string) {};
		std::function<void(std::string_view)> OpenFunction;
		std::function<void(uint64)> RootInputCommitted;
	};

	// Owns one document's transient material graph viewport and interaction state.
	class FMaterialGraphCanvas
	{
	public:
		// One immutable document binding for the canvas lifetime; never rebound on tab activation.
		explicit FMaterialGraphCanvas(DMaterialGraph& Graph, FMaterialGraphEditorServices Services = {});
		MATERIALEDITOR_API ~FMaterialGraphCanvas();
		// Release shared clipboard object references before the object system shuts down.
		static auto ClearSharedClipboard() -> void;

		auto Draw(
			DTransactor& Transactions,
			float Height) -> void;
		auto SelectAndFrame(const FGuid& NodeId) -> bool;
		auto DrawSelectionDetails() -> void;
		auto FinishDetailsEdit(bool bCancel) -> bool;
		auto DrawParameterValue(const FMaterialParameterDefinition& Parameter) -> void;
		enum class EParameterTextField { Name, DisplayName, Group, Binding };
		auto DrawParameterText(const char* Label, EParameterTextField Field, const FGuid& TargetId,
			std::string_view Value) -> void;
		auto ReplaceParameterDefinition(const DMaterialExpressionParameter& Expression,
			const FMaterialParameterDefinition& Definition) -> FMaterialGraphCommandResult;
		auto EndParameterFrame() -> void;
		auto EndNodeLiteralFrame() -> void;
		auto GetRootInputDraft() const -> const std::optional<std::pair<EMaterialProperty, FMaterialInputDefault>>&
		{
			return RootInputDraft;
		}
		auto SelectAndFrameDiagnostic(
			const FMaterialProgramDiagnostic& Diagnostic) -> bool;
		auto CancelInteraction() -> bool;
		// Sets detached viewport state for editor automation and rendered qualification.
		auto SetViewport(float InZoom, const ImVec2& InPan) -> void
		{
			Zoom = std::clamp(InZoom, 0.25f, 2.0f);
			Pan = InPan;
			DetailLevel = FMaterialGraphGeometry::SelectDetailLevel(
				Zoom, EMaterialGraphDetailLevel::Editing);
		}
		auto GetViewport() const -> std::pair<float, ImVec2> { return {Zoom, Pan}; }
		auto GetSelection() const -> const std::unordered_set<FMaterialGraphNodeAddress>&
		{
			return SelectedNodes;
		}
		void RestoreSelection(const std::unordered_set<FMaterialGraphNodeAddress>& Selection);
		auto GetSelectedSurfaceOutput() const
			-> std::optional<EMaterialProperty>
		{
			return SelectedSurfaceOutput;
		}

	private:
		auto CommitParameterValue(const FMaterialParameterDefinition& Parameter, const FMaterialParameterValue& Value) -> std::expected<void, std::string>;
		auto DrawParameterTexture(const FMaterialParameterDefinition& Parameter) -> bool;
		auto EndParameterTextFrame() -> void;
		auto EndRootInputFrame() -> void;
		auto FinishRootInputEdit(bool bCancel) -> bool;
		auto DrawRootInputValue(const FMaterialGraphPinView& Pin, uint64 Revision,
			FMaterialInputDefault Value, float Width) -> void;
		auto DrawNodeLiteral(const FMaterialGraphPin* Pin,
			EMaterialProgramValueType Type, const FMaterialProgramLiteral& Value) -> void;
		friend struct FMaterialGraphCanvasTestAccess;
		auto ReportError(std::string Message) const -> void;
		// Returns true for both Succeeded and NoChange.
		auto BreakPinLinks(const FMaterialGraphPin* Pin) -> bool;
		auto ConnectPins(const FMaterialGraphPin* Input, const FMaterialGraphPin* Source, bool Replace) -> bool;
		auto CheckCommand(const FMaterialGraphCommandResult& Result) const -> bool;
		template<class T> auto CheckCommand(const TMaterialGraphResult<T>& Result) const -> bool
		{
			return Result.has_value() || CheckCommand(Result.error());
		}
		TStrongObjectPtr<DMaterialGraph> EditorGraph;
		const FMaterialGraphEditorServices Services;
		struct FVisualNode;
		struct FVisualGraph;
		struct FPointerHit
		{
			const FVisualNode* Node = nullptr;
			const FVisualNode* InputNode = nullptr;
			const FVisualNode* OutputNode = nullptr;
			uint32 InputIndex = 0;
			size_t OutputIndex = 0;
			bool bPinExpansion = false;
		};

		auto HandleViewportInput(const ImVec2& Minimum, const ImVec2& Mouse, bool bHovered) -> void;
		auto HitTest(const FVisualGraph& VisualGraph, const ImVec2& Minimum,
			const ImVec2& Maximum, const ImVec2& Mouse) const -> FPointerHit;
		auto HandlePointerInput(DObject& Owner, DTransactor& Transactions,
			const FMaterialGraphView& View, const FVisualGraph& VisualGraph,
			const ImVec2& Minimum, const ImVec2& Maximum, const ImVec2& Size,
			const ImVec2& Mouse, bool bPointerAvailable) -> void;
		struct FTexturePreviewState;
		auto UpdateTexturePreviews(DMaterial& Material) -> void;
		auto DrawTexturePreview(const FGuid& NodeId, const ImVec2& Position, float Size) -> void;
		auto AcceptTextureDrop(DMaterial& Material, DTransactor& Transactions,
			const ImVec2& CanvasMinimum) -> void;

		struct FIdleInteraction {};
		struct FMovingInteraction
		{
			ImVec2 StartMouse{};
			bool bReleased = false;
			std::unordered_map<FGuid, FMaterialGraphNodePresentation> StartPositions;
		};
		struct FPinHandle
		{
			TWeakObjectPtr<DMaterialGraphNode> Node;
			FGuid PinId;
			explicit FPinHandle(const FMaterialGraphPin& Pin) : Node(Pin.GetOwningNode()), PinId(Pin.GetId()) {}
			auto Resolve(const DMaterialGraph& Graph) const -> const FMaterialGraphPin*
			{
				const auto* Owner = Node.Get();
				return Owner && Graph.FindNode(Owner->GetView().Node.Id) == Owner ? Owner->FindPinById(PinId) : nullptr;
			}
			auto operator==(const FPinHandle& Other) const -> bool { return PinId == Other.PinId && Node.Get() == Other.Node.Get(); }
		};
		struct FLinkingInteraction { FPinHandle Source; };
		struct FReconnectingInputInteraction
		{
			FPinHandle Destination;
		};

		struct FMarqueeInteraction { ImVec2 Start{}; };
		struct FNodeCreationMenuInteraction
		{
			std::optional<FPinHandle> Source;
			ImVec2 GraphPosition{};
			bool bOpenRequested = true;
			int32 Selection = 0;
			std::array<char, 96> Search{};
			std::array<char, 96> ParameterFilter{};
		};
		struct FContextMenuInteraction
		{
			FGuid ContextNode;
			std::optional<FPinHandle> RootInput;
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
			DTransactor& Transactions, const FMaterialGraphView& View,
			const ImVec2& CanvasMinimum, const ImVec2& CanvasSize,
			const ImVec2& Mouse, bool bInputAvailable) -> void;
		auto CopyNodes(DObject& Owner, std::span<const FGuid> NodeIds) -> void;
		auto CutNodes(DObject& Owner, DTransactor& Transactions,
			std::span<const FGuid> NodeIds) -> void;
		auto DuplicateNodes(DObject& Owner,
			DTransactor& Transactions, std::span<const FGuid> NodeIds) -> void;
		auto PasteNodes(DObject& Owner, DTransactor& Transactions,
			const ImVec2& GraphPosition) -> void;
		auto RemoveNodes(DObject& Owner, DTransactor& Transactions,
			std::span<const FGuid> NodeIds) -> void;
		auto DrawContextMenu(DObject& Owner,
			DTransactor& Transactions, const FMaterialGraphView& View) -> void;
		auto RememberCreation(const FMaterialGraphCreationAction& Node) -> void;
		auto HasClipboard() const -> bool;
		auto DrawCreationMenu(DObject& Owner, DTransactor& Transactions) -> void;
		auto ResetInteraction() -> void;
		auto ToggleNodePins(const FGuid& NodeId) -> void;
		auto PrepareFunctionView(DMaterialFunction& Function) -> void;
		auto PrepareDocumentView(DObject& Owner) -> void;
		auto GetSelectedDetailsNode() const -> DMaterialGraphNode*;
		auto HandleCreationShortcut(DObject& Owner, DTransactor& Transactions,
			const ImVec2& Position) -> bool;
		auto DrawNodeHeading(const FVisualNode& Visual, ImDrawList& DrawList,
			const DMaterial* Material = nullptr) const -> void;

		ImVec2 Pan{40.0f, 40.0f};
		float Zoom = 1.0f;
		bool bFunctionGraph = false;
		FGuid OutputNodeId;
		auto SelectionKey(FGuid Id) const -> FMaterialGraphNodeAddress
		{
			if (OutputNodeId.IsValid() && Id == OutputNodeId) return EMaterialGraphEndpoint::MaterialRoot;
			return Id;
		}
		EMaterialGraphDetailLevel DetailLevel = EMaterialGraphDetailLevel::Editing;
		std::unordered_set<FMaterialGraphNodeAddress> SelectedNodes;
		FGuid PendingFrameNode;
		std::optional<FMaterialProgramDiagnostic> SelectedDiagnostic;
		std::optional<EMaterialProperty> SelectedSurfaceOutput;
		std::vector<std::string> RecentCreationMenuEntries;
		FDelegateHandle GraphSynchronizedHandle;
		FMaterialGraphChangeSet PendingGraphChanges;
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
		std::unordered_set<FGuid> ExpandedPinNodes;
		std::unordered_set<FGuid> CollapsiblePinNodes;
		std::array<char, 129> PromotionNameDraft{};
		std::array<char, 256> NodeTextureSearch{};
		FInteraction Interaction = FIdleInteraction{};
		FMaterialGraphMoveSession MoveSession;
		FMaterialEditorPreviewParameters PreviewParameters;
		PropertyEditor::FPropertyControlFrame ParameterControls;
		struct FParameterTextDraft
		{
			EParameterTextField Field;
			FGuid TargetId;
			std::string Original;
			std::string Text;
			int ActiveFrame = -1;
		};
		std::optional<FParameterTextDraft> ParameterTextDraft;
		struct FNodeLiteralDraft
		{
			FPinHandle Pin;
			FMaterialInputDefault Value;
			uint64 Revision = 0;
			int ActiveFrame = -1;
		};
		std::optional<FNodeLiteralDraft> NodeLiteralDraft;
		std::unique_ptr<IDetailsView> ExpressionDetails = CreateDetailView();
		int ExpressionDetailsFrame = -1;
		using FRootInputControlId = std::pair<EMaterialProperty, ImGuiID>;
		PropertyEditor::TPropertyControlFrame<FRootInputControlId, uint64, FMaterialInputDefault> RootInputFrame;
		std::optional<std::pair<EMaterialProperty, FMaterialInputDefault>> RootInputDraft;
		uint64 RootInputRevision = 0;
	};
}
