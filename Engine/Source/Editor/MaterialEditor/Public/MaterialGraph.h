#pragma once

#include "CoreMinimal.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "MaterialGraphOperations.h"
#include "MaterialGraph.gen.h"

namespace Durin::MaterialEditor
{
	class FMaterialGraphSchema;

	DCLASS(NoClassDefaultObject)
	class DMaterialGraphNode : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DMaterialGraphNode(const FObjectInitializer& Initializer) : DObject(Initializer) {}
		auto GetView() const -> const FMaterialGraphNodeView& { return View; }
		MATERIALEDITOR_API auto AllocateDefaultPins() -> void;
		MATERIALEDITOR_API auto GetMaterialExpression() const -> DMaterialExpression*;
		MATERIALEDITOR_API auto ReconstructNode() -> void;
	protected:
		friend class DMaterialGraph;
		friend class FMaterialGraphReadModel;
		virtual auto CreateInputPins(std::vector<FMaterialGraphPinView> Pins) -> void { View.Inputs = std::move(Pins); }
		virtual auto CreateOutputPins(std::vector<FMaterialGraphOutputPinView> Pins) -> void { View.Outputs = std::move(Pins); }
	private:
		auto SetProjection(FMaterialGraphNodeView Projection) -> void;
		FMaterialGraphNodeView View;
	};

	DCLASS(NoClassDefaultObject)
	class DMaterialGraphNode_Root : public DMaterialGraphNode
	{
		GENERATED_BODY()
	public:
		explicit DMaterialGraphNode_Root(const FObjectInitializer& Initializer) : DMaterialGraphNode(Initializer) {}
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
		auto GetNodes() const -> std::span<const TObjectPtr<DMaterialGraphNode>> { return Nodes; }
		MATERIALEDITOR_API auto FindNode(const FGuid& Id) const -> DMaterialGraphNode*;
		MATERIALEDITOR_API auto FindInputPin(const FMaterialGraphPinAddress& Address) const -> const FMaterialGraphPinView*;
		MATERIALEDITOR_API auto FindOutputPin(const FMaterialProgramLink& Link) const -> const FMaterialGraphOutputPinView*;
		MATERIALEDITOR_API auto AddExpression(DMaterialExpression& Expression) -> DMaterialGraphNode*;
		MATERIALEDITOR_API auto RebuildGraph() -> void;
		MATERIALEDITOR_API auto LinkGraphNodesFromMaterial() -> void;
	private:
		friend class FMaterialGraphReadModel;
		friend class FMaterialGraphSchema;
		auto ApplyProjection(FMaterialGraphView Snapshot) -> void;
		auto LinkMaterialExpressionsFromGraph(const FMaterialGraphPinAddress& Input, const FMaterialGraphPinAddress& Output,
			bool Replace, ETransactionMode Mode) -> FMaterialGraphCommandResult;
		auto AppendNode(FMaterialGraphNodeView Projection) -> DMaterialGraphNode*;
		TWeakObjectPtr<DObject> Owner;
		const FMaterialGraphSchema* Schema = nullptr;
		DPROPERTY(Transient, NonTransactional)
		std::vector<TObjectPtr<DMaterialGraphNode>> Nodes;
	};
}
