#pragma once

#include "CoreMinimal.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "MaterialGraphDocument.h"
#include "Materials/MaterialGraphChanges.h"
#include "Delegates/Delegate.h"
#include "MaterialGraph.gen.h"

namespace Durin::MaterialEditor
{
	class FMaterialGraphSchema;
	struct FMaterialGraphPinSource;

	DCLASS(NoClassDefaultObject)
	class DMaterialGraphNode : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DMaterialGraphNode(const FObjectInitializer& Initializer) : DObject(Initializer) {}
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
		auto ResolveInputGraph(const FMaterialGraphPinAddress& Pin) const -> DMaterialGraph*;
		friend MATERIALEDITOR_API auto ProjectMaterialGraph(const DObject*, std::span<const FMaterialGraphCatalogEntry>,
			std::optional<std::span<const FGuid>>) -> FMaterialGraphView;
		static auto BuildInputPins(FMaterialGraphNodeView& View, const FMaterialGraphPinSource& Source) -> void;
		static auto BuildOutputPins(FMaterialGraphNodeView& View, const FMaterialGraphPinSource& Source) -> void;
		friend class DMaterialGraphNode_Root;
		using FPinOperation = void (DMaterialGraphNode::*)();
		auto Refresh(FMaterialGraphNodeView Metadata, const FMaterialGraphPinSource& Source, FPinOperation Operation) -> void;
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
	private:
		friend MATERIALEDITOR_API auto ProjectMaterialGraph(const DObject*, std::span<const FMaterialGraphCatalogEntry>,
			std::optional<std::span<const FGuid>>) -> FMaterialGraphView;
		static auto BuildInputPins(FMaterialGraphNodeView& View, const FMaterialGraphPinSource& Source) -> void;
	};

	DCLASS(NoClassDefaultObject)
	class DMaterialGraph : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DMaterialGraph(const FObjectInitializer& Initializer) : DObject(Initializer) {}
		MATERIALEDITOR_API static auto Create(DObject& MaterialOrFunction) -> DMaterialGraph*;
		auto GetMaterialOrFunction() const -> DObject* { return Owner.Get(); }
		auto GetCommands() const -> FMaterialGraphDocument { return FMaterialGraphDocument(Owner, GetSchema()); }
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
		auto LinkMaterialExpressionsFromGraph(const FMaterialGraphPinAddress& Input, const FMaterialGraphPinAddress& Output,
			bool Replace, ETransactionMode Mode) -> FMaterialGraphCommandResult;
		auto BreakOutputLinks(const FMaterialProgramLink& Link, ETransactionMode Mode) -> FMaterialGraphCommandResult;
		TWeakObjectPtr<DObject> Owner;
		const FMaterialGraphSchema* Schema = nullptr;
		DPROPERTY(Transient, NonTransactional)
		std::vector<TObjectPtr<DMaterialGraphNode>> Nodes;
	};
}
