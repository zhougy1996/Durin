#pragma once

#include "CoreMinimal.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "MaterialGraphSchema.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialGraphChanges.h"
#include "Delegates/Delegate.h"
#include "MaterialGraph.gen.h"

namespace Durin::MaterialEditor
{
	class FMaterialGraphSchema;
	struct FMaterialGraphPinSource;

	class DMaterialGraphNode;
	enum class EMaterialGraphPinDirection : uint8 { Input, Output };

	// Node-owned editor pins. Borrow until the node is reconstructed or removed.
	struct FMaterialGraphPin
	{
		EMaterialGraphPinDirection Direction;
		int32 SourceIndex;
		int32 DisplayIndex;
		DMaterialGraphNode* GetOwningNode() const { return OwningNode; }
		std::vector<const FMaterialGraphPin*> LinkedTo;
	private:
		friend class DMaterialGraphNode;
		friend class DMaterialGraph;
		FMaterialGraphPin(DMaterialGraphNode* Owner, EMaterialGraphPinDirection InDirection, int32 Index, int32 Ordinal)
			: Direction(InDirection), SourceIndex(Index), DisplayIndex(Ordinal), OwningNode(Owner) {}
		auto MakeLinkTo(FMaterialGraphPin& Other) -> void
		{
			LinkedTo.push_back(&Other);
			Other.LinkedTo.push_back(this);
		}
		DMaterialGraphNode* OwningNode;
	};

	DCLASS(NoClassDefaultObject)
	class DMaterialGraphNode : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DMaterialGraphNode(const FObjectInitializer& Initializer) : DObject(Initializer) {}
		auto GetPins() const -> std::span<const FMaterialGraphPin> { return Pins; }
		// Display ordinals select pins; SourceIndex selects a slot in the underlying expression.
		MATERIALEDITOR_API auto GetInputPin(int32 InputIndex) const -> const FMaterialGraphPin*;
		MATERIALEDITOR_API auto GetOutputPin(int32 OutputIndex) const -> const FMaterialGraphPin*;
		auto GetView() const -> const FMaterialGraphNodeView& { return View; }
		MATERIALEDITOR_API virtual auto AllocateDefaultPins() -> void;
		MATERIALEDITOR_API auto GetMaterialExpression() const -> DMaterialExpression*;
		MATERIALEDITOR_API virtual auto OnRenameNode(std::string NewName,
			ETransactionMode Mode = ETransactionMode::Record) -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto SetFunctionPort(FMaterialFunctionPort Port,
			ETransactionMode Mode = ETransactionMode::Record) -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto SetConstantValue(const FMaterialParameterValue& Value,
			ETransactionMode Mode = ETransactionMode::Record) -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto SetSwizzleComponents(std::span<const uint8> Components,
			ETransactionMode Mode = ETransactionMode::Record) -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto CanSetAttributeInput(EMaterialProperty Property, const FMaterialGraphPin* Output) const -> FMaterialGraphConnectionResponse;
		MATERIALEDITOR_API auto SetAttributeInput(EMaterialProperty Property, const FMaterialGraphPin& Output,
			ETransactionMode Mode = ETransactionMode::Record, std::string* OutError = nullptr) -> bool;
		MATERIALEDITOR_API auto ClearAttributeInput(EMaterialProperty Property,
			ETransactionMode Mode = ETransactionMode::Record) -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto SetAttributeOutputEnabled(FGuid AttributeId, bool Enabled,
			ETransactionMode Mode = ETransactionMode::Record) -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto ExtractInputDefault(const FMaterialGraphPinAddress& Pin,
			ETransactionMode Mode = ETransactionMode::Record) -> TMaterialGraphResult<DMaterialGraphNode*>;
		MATERIALEDITOR_API auto InlineInputNode(const FMaterialGraphPinAddress& Pin,
			ETransactionMode Mode = ETransactionMode::Record) -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto SetPinUseConstant(const FMaterialGraphPinAddress& Pin, bool Enabled,
			ETransactionMode Mode = ETransactionMode::Record) -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API virtual auto PinDefaultValueChanged(const FMaterialGraphPinAddress& Pin, FMaterialInputDefault Value,
			ETransactionMode Mode = ETransactionMode::Record) -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API virtual auto ReconstructNode() -> void;
	protected:
		friend class DMaterialGraph;
		MATERIALEDITOR_API virtual auto CreateInputPins() -> void;
		MATERIALEDITOR_API virtual auto CreateOutputPins() -> void;
		FMaterialGraphNodeView View;
	private:
		std::vector<FMaterialGraphPin> Pins;
		TWeakObjectPtr<DMaterialExpression> MaterialExpression;
		auto ClearPinLinks() -> void
		{
			for (auto& Pin : Pins) Pin.LinkedTo.clear();
		}
		auto ResolveInputGraph(const FMaterialGraphPinAddress& Pin) const -> DMaterialGraph*;
		friend class DMaterialGraphNode_Root;
		using FPinOperation = void (DMaterialGraphNode::*)();
		auto Refresh(FMaterialGraphNodeView Metadata, FMaterialGraphPinSource Source, FPinOperation Operation) -> void;
		auto CreatePins() -> void;
		// Available only while dispatching the synchronous allocation/reconstruction hooks.
		const FMaterialGraphPinSource* ActiveSource = nullptr;
	};

	DCLASS(NoClassDefaultObject)
	class DMaterialGraphNode_Root : public DMaterialGraphNode
	{
		GENERATED_BODY()
	public:
		explicit DMaterialGraphNode_Root(const FObjectInitializer& Initializer) : DMaterialGraphNode(Initializer) {}
	protected:
		MATERIALEDITOR_API auto CreateInputPins() -> void override;
		auto CreateOutputPins() -> void override {}
	};

	DCLASS(NoClassDefaultObject)
	class DMaterialGraph : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DMaterialGraph(const FObjectInitializer& Initializer) : DObject(Initializer) {}
		MATERIALEDITOR_API static auto Create(DObject& MaterialOrFunction) -> DMaterialGraph*;
		auto GetMaterialOrFunction() const -> DObject* { return Owner.Get(); }
		MATERIALEDITOR_API auto GetSchema() const -> const FMaterialGraphSchema&;
		auto GetRootNode() const -> DMaterialGraphNode* { return FindNode(RootViewId); }
		auto GetNodes() const -> std::span<const TObjectPtr<DMaterialGraphNode>> { return Nodes; }
		MATERIALEDITOR_API auto FindNode(const FGuid& Id) const -> DMaterialGraphNode*;
		auto ResolveNode(const FMaterialGraphNodeAddress& Address) const -> DMaterialGraphNode*
		{
			if (const auto* Id = std::get_if<FGuid>(&Address)) return FindNode(*Id);
			return GetRootNode();
		}
		MATERIALEDITOR_API auto FindPin(const FMaterialGraphPinAddress& Address) const -> const FMaterialGraphPin*;
		MATERIALEDITOR_API auto FindInputPin(const FMaterialGraphPinAddress& Address) const -> const FMaterialGraphPinView*;
		MATERIALEDITOR_API auto FindOutputPin(const FMaterialProgramLink& Link) const -> const FMaterialGraphOutputPinView*;
		// Semantic position commands resolve against the receiving graph, including after reload.
		MATERIALEDITOR_API auto MoveSelection(std::span<const FMaterialGraphNodePosition> Positions,
			ETransactionMode Mode = ETransactionMode::Record, std::string* OutError = nullptr) const -> bool;
		// Position commands ignore labels and preserve unrelated presentation fields.
		MATERIALEDITOR_API auto MoveNodes(std::span<const FMaterialGraphNodePresentation> Positions,
			ETransactionMode Mode = ETransactionMode::Record, std::string* OutError = nullptr) const -> bool;
		MATERIALEDITOR_API auto CalculateLayout(std::span<const FGuid> NodeIds,
			FMaterialGraphPresentation& OutPresentation, std::string* OutError = nullptr) const -> bool;
		MATERIALEDITOR_API auto Layout(std::span<const FGuid> NodeIds = {},
			ETransactionMode Mode = ETransactionMode::Record, std::string* OutError = nullptr) const -> bool;
		MATERIALEDITOR_API auto CopySelection(std::span<const FGuid> NodeIds,
			FMaterialGraphClipboardPayload& OutPayload, std::string* OutError = nullptr) const -> bool;
		MATERIALEDITOR_API auto Paste(const FMaterialGraphClipboardPayload& Payload,
			int32 X, int32 Y, ETransactionMode Mode = ETransactionMode::Record) -> TMaterialGraphResult<std::vector<DMaterialGraphNode*>>;
		MATERIALEDITOR_API auto DuplicateNodes(std::span<const FGuid> NodeIds,
			int32 OffsetX = 40, int32 OffsetY = 40, ETransactionMode Mode = ETransactionMode::Record) -> TMaterialGraphResult<std::vector<DMaterialGraphNode*>>;
		MATERIALEDITOR_API auto CutSelection(std::span<const FGuid> NodeIds,
			FMaterialGraphClipboardPayload& OutPayload, ETransactionMode Mode = ETransactionMode::Record) -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto PromoteInputToParameter(const FMaterialGraphPinAddress& Target,
			ETransactionMode Mode = ETransactionMode::Record) -> TMaterialGraphResult<std::vector<DMaterialGraphNode*>>;
		MATERIALEDITOR_API auto AddPort(bool bOutput, FMaterialFunctionPort Port,
			FMaterialProgramLink Source = {}, int32 X = 0, int32 Y = 0,
			ETransactionMode Mode = ETransactionMode::Record) -> TMaterialGraphResult<DMaterialGraphNode*>;
		MATERIALEDITOR_API auto RemovePort(bool bOutput, const FGuid& PortId,
			ETransactionMode Mode = ETransactionMode::Record) -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto CreateExpression(const DMaterialExpression& Expression, int32 X = 0, int32 Y = 0,
			ETransactionMode Mode = ETransactionMode::Record) -> TMaterialGraphResult<DMaterialGraphNode*>;
		MATERIALEDITOR_API auto ReplaceExpression(const DMaterialExpression& Expression,
			ETransactionMode Mode = ETransactionMode::Record) -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto InsertFunctionCall(DMaterialFunctionInterface& Function,
			int32 X, int32 Y, ETransactionMode Mode = ETransactionMode::Record) -> TMaterialGraphResult<DMaterialGraphNode*>;
		MATERIALEDITOR_API auto InsertFunctionCall(DMaterialFunctionInterface& Function,
			int32 X, int32 Y, std::span<const FMaterialFunctionInputBinding> Inputs,
			ETransactionMode Mode = ETransactionMode::Record) -> TMaterialGraphResult<DMaterialGraphNode*>;
		MATERIALEDITOR_API auto RemoveNodes(std::span<const FGuid> NodeIds,
			ETransactionMode Mode = ETransactionMode::Record) -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto AddExpression(DMaterialExpression& Expression) -> DMaterialGraphNode*;
		MATERIALEDITOR_API auto RebuildGraph() -> void;
		// Relinks the already allocated pins without changing their addresses.
		MATERIALEDITOR_API auto LinkGraphNodesFromMaterial() -> void;
	private:
		friend class DMaterialGraphNode;
		friend struct FMaterialGraphCreationAction;
		auto GetCreatedNode(const FGuid& Id) -> TMaterialGraphResult<DMaterialGraphNode*>;
		auto GetCreatedNodes(std::span<const FGuid> Ids) -> TMaterialGraphResult<std::vector<DMaterialGraphNode*>>;
		auto CreateCatalogNode(const FMaterialGraphCatalogEntry& Entry, int32 X, int32 Y,
			const FMaterialGraphPin* Source, DTransactor* Transactions, std::vector<DMaterialGraphNode*>* AuxiliaryNodes) -> TMaterialGraphResult<DMaterialGraphNode*>;
		auto CalculateLayoutPositions(std::span<const FGuid> NodeIds, const FMaterialGraphPresentation& Presentation,
			std::vector<FMaterialGraphNodePresentation>& Positions, std::string* OutError) const -> bool;
		friend class FMaterialGraphMoveSession;
		auto CommitPositions(std::span<const FMaterialGraphNodePresentation> Positions,
			DTransactor* Transactions, bool bAuthorDefaultPositions, std::string_view Description, std::string* OutError) const -> bool;
		auto MoveNodesInternal(std::span<const FMaterialGraphNodePresentation> Positions,
			DTransactor* Transactions, std::string* OutError) const -> bool;
		friend class FMaterialGraphReadModel;
		auto HasExpressionConsumer(const DMaterialExpression& Source) const -> bool;
		auto RestorePinLinks() -> void;
		friend class FMaterialGraphSchema;
		MATERIALEDITOR_API auto BeginDestroy() -> void override;
		auto ApplyChanges(FMaterialGraphChangeSet Changes, bool CheckDependencies = true) -> void;
		auto ObserveDependencies(DObject& Asset) -> void;
		auto UnsubscribeDependencies() -> void;
		struct FSubscription
		{
			TWeakObjectPtr<DObject> Owner;
			FDelegateHandle Handle;
		};
		FDelegateHandle OwnerHandle;
		std::vector<FSubscription> Dependencies;
		DECLARE_MULTICAST_DELEGATE_OneParam(FGraphSynchronized, const FMaterialGraphChangeSet&)
		FGraphSynchronized Synchronized;
		FMaterialAttributeValues Outputs;
		auto RefreshNodes(std::span<const FMaterialGraphCatalogEntry> Catalog,
			std::optional<std::span<const FGuid>> Selected = {}) -> void;
		auto LinkMaterialExpressionsFromGraph(const FMaterialGraphPin& Input, const FMaterialGraphPin* Output,
			bool Replace, ETransactionMode Mode, std::string* OutError) -> bool;
		auto BreakOutputLinks(const FMaterialGraphPin& Output, ETransactionMode Mode, std::string* OutError) -> bool;
		// Local widget handle; persistent commands address the root by kind and material property.
		FGuid RootViewId = FGuid::NewGuid();
		TWeakObjectPtr<DObject> Owner;
		const FMaterialGraphSchema* Schema = nullptr;
		DPROPERTY(Transient, NonTransactional)
		std::vector<TObjectPtr<DMaterialGraphNode>> Nodes;
	};
}
