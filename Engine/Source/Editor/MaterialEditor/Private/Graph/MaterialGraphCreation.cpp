#include "MaterialExpressionParameters.h"
#include "Graph/MaterialGraphCatalog.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionVector.h"
#include "Materials/MaterialExpressionMaterialAttributes.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionFunctionIO.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionSwitch.h"
#include "Materials/MaterialExpressionTextureParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Misc/UnifiedError.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionBuild.h"
#include "Materials/MaterialParameterCollection.h"
#include "MaterialGraph.h"
#include "MaterialGraphEditInternals.h"
#include "Materials/MaterialExpressionInputs.h"
#include "MaterialGraphExpressionRegistry.h"
#include "MaterialGraphEditSession.h"
#include "DObject/Package.h"
#include "MaterialGraphValueTypes.h"
#include "Asset/Asset.h"

namespace Durin::MaterialEditor
{
	using namespace GraphEditInternals;

	namespace
	{
		struct FCreationFunctionResult
		{
			DMaterialFunctionInterface* Function = nullptr;
			std::string Message;
			explicit operator bool() const { return Function != nullptr; }
		};
		auto LoadCreationFunction(const std::string& Name) -> FCreationFunctionResult
		{
			std::optional<Durin::UnifiedError::FError> LoadedError;
			FTopLevelAssetPath Path;
			if (const auto Parsed = FTopLevelAssetPath::TryCreateWithDiagnostic(Name, Path); !Parsed)
				return {.Message = "The material function path is invalid. " + ToString(Parsed.error())};
			DMaterialFunctionInterface* Function = nullptr;
			if (auto Loaded = LoadObject<DMaterialFunctionInterface>(Path, nullptr, &LoadedError); !Loaded)
				return {.Message = "Unable to load the material function. " + LoadedError->ToString()};
			else { Function = Loaded; }
			return {.Function = Function};
		}
	}
	auto FMaterialGraphSchema::ForOwner(const DObject& Owner) -> const FMaterialGraphSchema&
	{
		static const FMaterialSchema Material;
		static const FMaterialFunctionSchema Function;
		if (Cast<DMaterialFunction>(&Owner)) return Function;
		if (Cast<DMaterial>(&Owner)) return Material;
		check(false); std::terminate();
	}

	auto FMaterialGraphSchema::CanCreate(const FMaterialGraphCreationAction& Action,
		std::optional<EMaterialProgramValueType> SourceType) const -> bool
	{
		if (!AllowsCreation(Action, SourceType)) return false;
		if (!std::holds_alternative<std::string>(Action.Payload)) return true;
		if (!SourceType) return true;
		const auto Loaded = LoadCreationFunction(std::get<std::string>(Action.Payload));
		return Loaded && std::ranges::any_of(Loaded.Function->GetFunctionSignature().Inputs,
			[&](const auto& Port) { return FMaterialGraphSchema::AcceptsPort(Port.Type, *SourceType); });
	}
	auto FMaterialGraphCreationAction::PerformAction(DMaterialGraph& Graph, int32 X, int32 Y,
		std::optional<FMaterialGraphPinAddress> SourcePin, ETransactionMode Mode,
			std::vector<DMaterialGraphNode*>* AuxiliaryNodes) const -> TMaterialGraphResult<DMaterialGraphNode*>
	{
		if (AuxiliaryNodes) AuxiliaryNodes->clear();
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return std::unexpected(RejectCommand(Admission.error()));
		if (!Graph.GetMaterialOrFunction()) return std::unexpected(RejectCommand("The graph owner is unavailable.", {}, EMaterialGraphCommandStatus::StaleOwner));
		FMaterialProgramLink Source;
		std::optional<EMaterialProgramValueType> SourceType;
		const FMaterialGraphPin* Pin = nullptr;
		if (SourcePin)
		{
			if (SourcePin->Kind != EMaterialGraphPinKind::Output && SourcePin->Kind != EMaterialGraphPinKind::FunctionOutput)
				return std::unexpected(RejectCommand("Creation requires an output pin."));
			Pin = Graph.FindPin(*SourcePin);
			if (!Pin) return std::unexpected(RejectCommand("The source output no longer exists."));
			const auto& Output = Pin->GetOwningNode()->GetView().Outputs[Pin->DisplayIndex];
			SourceType = Output.Type;
			Source = {SourcePin->NodeId, Output.OutputIndex, Output.PortId};
		}
		if (!Graph.GetSchema().AllowsCreation(*this, SourceType))
			return std::unexpected(RejectCommand("This action is not compatible with the graph or source output."));
		if (const auto* Entry = std::get_if<FMaterialGraphCatalogEntry>(&Payload))
			return Graph.CreateCatalogNode(*Entry, X, Y, Pin, *Admission, AuxiliaryNodes);
		if (const auto* Spec = std::get_if<FMaterialGraphPortCreation>(&Payload))
		{
			const auto* Function = Cast<DMaterialFunction>(Graph.GetMaterialOrFunction());
			if (!Function) return std::unexpected(RejectCommand("Only function graphs have ports."));
			const auto& Signature = Function->GetFunctionSignature();
			const auto& Ports = Spec->bOutput ? Signature.Outputs : Signature.Inputs;
			FMaterialFunctionPort Port;
			Port.Type = Spec->Type;
			for (uint32 Index = 1;; ++Index)
			{
				Port.Name = std::format("{} {}", Spec->bOutput ? "Output" : "Input", Index);
				if (std::ranges::none_of(Ports, [&](const auto& Existing) { return Existing.Name == Port.Name; })) break;
			}
			if (!Spec->bOutput)
				Port.Default.Kind = Port.Type == EMaterialProgramValueType::MaterialAttributes ? EMaterialFunctionDefaultKind::MaterialAttributes
					: Port.Type == EMaterialProgramValueType::Texture2D ? EMaterialFunctionDefaultKind::Texture
					: EMaterialFunctionDefaultKind::Numeric;
			return Graph.AddPort(Spec->bOutput, Port, Source, X, Y, Mode);
		}
		if (const auto* Spec = std::get_if<
			FMaterialGraphCollectionParameterCreation>(&Payload))
		{
			FTopLevelAssetPath Path;
			if (!FTopLevelAssetPath::TryCreateWithDiagnostic(
				Spec->CollectionPath, Path))
				return std::unexpected(RejectCommand("The collection asset path is invalid."));
			auto Loaded = LoadObject<DMaterialParameterCollection>(Path);
			if (!Loaded) return std::unexpected(RejectCommand(
				"The collection asset could not be loaded."));
			if (!Loaded->FindDeclaration(Spec->ParameterId))
				return std::unexpected(RejectCommand("The selected collection declaration no longer exists."));
			TStrongObjectPtr<DMaterialExpressionCollectionParameter> Expression(
				NewObject<DMaterialExpressionCollectionParameter>(nullptr, NAME_None));
			Expression->Id = FGuid::NewGuid();
			Expression->Collection = Loaded;
			Expression->ParameterId = Spec->ParameterId;
			return Graph.CreateExpression(*Expression, X, Y, Mode);
		}
		auto Loaded = LoadCreationFunction(std::get<std::string>(Payload));
		if (!Loaded)
		{
			return std::unexpected(RejectCommand(std::move(Loaded.Message)));
		}
		auto* Function = Loaded.Function;
		std::vector<FMaterialFunctionInputBinding> Inputs;
		if (SourceType)
		{
			for (const auto& Port : Function->GetFunctionSignature().Inputs)
				if (FMaterialGraphSchema::AcceptsPort(Port.Type, *SourceType)) { Inputs.push_back({Port.Id, Port.Type, Source}); break; }
			if (Inputs.empty()) return std::unexpected(RejectCommand("This function has no compatible input."));
		}
		return Graph.InsertFunctionCall(*Function, X, Y, Inputs, Mode);
	}
	auto DMaterialGraph::GetCreatedNode(const FGuid& Id) -> TMaterialGraphResult<DMaterialGraphNode*>
	{
		if (auto* Node = FindNode(Id)) return Node;
		// An outer change batch can defer the notification that normally creates these nodes.
		if (Owner.IsValid())
		{
			RefreshNodes(EnumerateCatalog());
			if (auto* Node = FindNode(Id)) return Node;
		}
		return std::unexpected(RejectCommand("The created graph node is no longer available."));
	}

	auto DMaterialGraph::GetCreatedNodes(std::span<const FGuid> Ids) -> TMaterialGraphResult<std::vector<DMaterialGraphNode*>>
	{
		std::vector<DMaterialGraphNode*> Created;
		Created.reserve(Ids.size());
		for (const auto& Id : Ids)
		{
			auto Node = GetCreatedNode(Id);
			if (!Node) return std::unexpected(std::move(Node.error()));
			Created.push_back(*Node);
		}
		return Created;
	}

	auto DMaterialGraph::AddPort(bool bOutput, FMaterialFunctionPort Port,
		FMaterialProgramLink Source, int32 X, int32 Y, ETransactionMode Mode) -> TMaterialGraphResult<DMaterialGraphNode*>
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return std::unexpected(RejectCommand(Admission.error()));
		auto* Transactions = *Admission;
		if (!Owner.IsValid()) return std::unexpected(RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner));
		auto* Function = Cast<DMaterialFunction>(Owner.Get());
		if (!Function) return std::unexpected(RejectCommand("Only function documents have an interface."));
		FGraphEditSession State(*Function);
		if (!Port.Id.IsValid()) Port.Id = FGuid::NewGuid();
		const auto NodeId = FGuid::NewGuid();
		if (bOutput)
		{
			auto* Terminal = NewObject<DMaterialExpressionFunctionOutput>(nullptr, NAME_None);
			Terminal->Id = NodeId; Terminal->Port = std::move(Port);
			auto* Upstream = FindMaterialExpression(State.Expressions, Source.SourceNodeId);
			if (Source.SourceNodeId.IsValid() && !Upstream) return std::unexpected(RejectCommand("The source expression no longer exists."));
			State.IncludeCallOutput(Upstream, Source.SourceOutputId);
			Terminal->Source = {Upstream, Source.SourceOutputIndex, Source.SourceOutputId};
			State.Expressions.emplace_back(Terminal);
		}
		else
		{
			auto* Terminal = NewObject<DMaterialExpressionFunctionInput>(nullptr, NAME_None);
			Terminal->Id = NodeId; Terminal->Port = std::move(Port);
			State.Expressions.emplace_back(Terminal);
		}
		State.Presentation.Nodes.push_back({NodeId, X, Y});
		auto Result = State.Commit(
			bOutput ? "Add Function Output" : "Add Function Input", Transactions);
		if (!Result) return std::unexpected(std::move(Result));
		return GetCreatedNode(NodeId);
	}

	auto DMaterialGraph::RemovePort(bool bOutput, const FGuid& PortId,
		ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		auto* Function = Cast<DMaterialFunction>(Owner.Get());
		if (!Function) return RejectCommand("Only function documents have an interface.");
		FGraphEditSession State(*Function);
		const auto Removed = std::erase_if(State.Expressions, [&](const auto& Expression) {
			if (bOutput)
			{
				const auto* Terminal = Cast<DMaterialExpressionFunctionOutput>(Expression.Get());
				return Terminal && Terminal->Port.Id == PortId;
			}
			const auto* Terminal = Cast<DMaterialExpressionFunctionInput>(Expression.Get());
			return Terminal && Terminal->Port.Id == PortId;
		});
		if (!Removed) return {.Status = EMaterialGraphCommandStatus::NoChange};
		return State.Commit("Remove Function Port", Transactions);
	}

	auto DMaterialGraph::CreateCatalogNode(const FMaterialGraphCatalogEntry& Entry, int32 X, int32 Y,
		const FMaterialGraphPin* Source, DTransactor* Transactions, std::vector<DMaterialGraphNode*>* AuxiliaryNodes) -> TMaterialGraphResult<DMaterialGraphNode*>
	{
		const auto* Registration = FindMaterialGraphExpressionRegistration(Entry.ExpressionClass);
		if (!Registration || Registration->CreationKind != EMaterialGraphCreationKind::Direct)
			return std::unexpected(RejectCommand("The catalog expression class is unavailable for direct creation."));
		const auto& Description = *Registration->Description;
		const auto Shape = std::ranges::find(Description.Shapes, Entry.ResultType, &FMaterialExpressionAuthoringShape::ResultType);
		if (Shape == Description.Shapes.end() || Entry.AcceptedInputTypes != Shape->AcceptedInputTypes
			|| Description.SemanticOpcode != Entry.Opcode)
			return std::unexpected(RejectCommand("The catalog expression shape is stale."));
		FGraphEditSession State(*Owner.Get());
		FMaterialExpressionInput FirstInput;
		if (Source)
		{
			auto* Node = Source->GetOwningNode();
			auto* Expression = Node->GetMaterialExpression();
			const auto& Output = Node->GetView().Outputs[Source->DisplayIndex];
			if (Source->SourceIndex >= 0) FirstInput.Connect(Source->SourceIndex, Expression);
			else
			{
				State.IncludeCallOutput(Expression, Output.PortId);
				FirstInput = {Expression, 0, Output.PortId};
				if (FirstInput.OutputIndex < 0) return std::unexpected(RejectCommand("The source output no longer exists."));
			}
		}
		if (FirstInput.IsConnected() && Entry.AcceptedInputTypes.empty())
			return std::unexpected(RejectCommand("This catalog expression has no input pin."));
		auto MakeUniqueStaticBoolName = [&]() {
			const std::string BaseName = "StaticBool";
			FName Candidate(BaseName);
			for (uint32 Suffix = 1; std::ranges::any_of(State.Expressions, [&](const auto& Value) {
				const auto* Declaration = Cast<DMaterialExpressionStaticBool>(Value.Get());
				return Declaration && Declaration->Name == Candidate;
			}); ++Suffix)
				Candidate = FName(std::format("{}{}", BaseName, Suffix));
			return Candidate;
		};
		FGuid GeneratedStaticBoolId;
		if (Entry.Opcode == EMaterialProgramOpcode::StaticSwitch
			&& !FirstInput.IsConnected())
		{
			TStrongObjectPtr<DMaterialExpressionStaticBool> Condition(
				NewObject<DMaterialExpressionStaticBool>(nullptr, NAME_None));
			Condition->Id = GeneratedStaticBoolId = FGuid::NewGuid();
			Condition->DeclarationId = FGuid::NewGuid();
			Condition->Name = MakeUniqueStaticBoolName();
			FirstInput = {Condition.Get()};
			State.Presentation.Nodes.push_back({Condition->Id, X - 220, Y, "StaticBool"});
			State.Expressions.emplace_back(Condition.Get());
		}
		TStrongObjectPtr<DMaterialExpression> Expression(NewRegisteredMaterialExpression(Entry.ExpressionClass));
		if (!Expression) return std::unexpected(RejectCommand("The catalog expression class is unavailable."));
		Expression->Id = FGuid::NewGuid();
		if (auto* Declaration = Cast<DMaterialExpressionStaticBool>(Expression.Get()))
		{
			Declaration->DeclarationId = FGuid::NewGuid();
			Declaration->Name = MakeUniqueStaticBoolName();
		}
		if (auto* Property = Expression->GetClass()->FindPropertyByName("ResultType"))
			*static_cast<EMaterialProgramValueType*>(Property->GetValuePtr(Expression.Get())) = Entry.ResultType;
		if (auto* Swizzle = Cast<DMaterialExpressionSwizzle>(Expression.Get()))
		{
			if (Entry.ResultType > EMaterialProgramValueType::Float4) return std::unexpected(RejectCommand("The swizzle width is invalid."));
			Swizzle->Components.clear();
			for (uint8 Index = 0; Index <= static_cast<uint8>(Entry.ResultType); ++Index) Swizzle->Components.push_back(Index);
		}
		std::string DisplayName;
		if (auto* Texture = Cast<DMaterialExpressionTextureParameter>(Expression.Get()); Texture && Entry.ResultType == EMaterialProgramValueType::TextureCube)
			Texture->DefaultValue.Dimension = EMaterialTextureDimension::TextureCube;
		if (auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get()))
		{
			if (!GetSchema().CanOwnParameters()) return std::unexpected(RejectCommand("Functions cannot own root parameters."));
			Parameter->Metadata.Id = FGuid::NewGuid();
			const bool bTexture = Cast<DMaterialExpressionTextureParameter>(Expression.Get()) != nullptr;
			const auto BaseName = bTexture ? std::string("TextureParameter") : std::string(GetProgramTypeName(Entry.ResultType)) + "Parameter";
			Parameter->Metadata.Name = FName(BaseName);
			const auto* Material = Cast<DMaterial>(Owner.Get());
			for (uint32 Suffix = 1; Material->FindParameterDefinition(Parameter->Metadata.Name); ++Suffix)
				Parameter->Metadata.Name = FName(std::format("{}{}", BaseName, Suffix));
			DisplayName = Parameter->Metadata.DisplayName = Parameter->Metadata.Name.ToString();
		}
		if (Entry.AcceptedInputTypes.size() != Expression->GetAuthoredInputCount())
			return std::unexpected(RejectCommand("The catalog input shape is stale."));
		std::optional<uint32> InvalidInput;
		if (auto* Blend = Cast<DMaterialExpressionBlendMaterialAttributes>(Expression.Get())) Blend->B = FirstInput;
		VisitMaterialExpressionInputs(*Expression, [&](uint32 Index, FMaterialExpressionInput& Input) {
			if (Index == 0 && FirstInput.IsConnected())
			{
				Input = FirstInput;
				if (!IsMaterialAdaptiveNumeric(Entry.Opcode)) return;
			}
			if (const auto* Sample = Cast<DMaterialExpressionTextureSample2D>(Expression.Get()); Sample && &Input == &Sample->UV.Connection) return;
			if (const auto* Sample = Cast<DMaterialExpressionTextureSampleParameter2D>(Expression.Get()); Sample && &Input == &Sample->UV.Connection) return;
			if ((Entry.Opcode == EMaterialProgramOpcode::QualitySwitch
				|| Entry.Opcode == EMaterialProgramOpcode::FeatureLevelSwitch)
				&& Index > 0) return;
			if (Entry.Opcode != EMaterialProgramOpcode::MakeMaterialAttributes && Entry.Opcode != EMaterialProgramOpcode::SetMaterialAttributes
				&& !FindMaterialNumericInput(*Expression, Input) && !Input.IsConnected())
				if (!InvalidInput) InvalidInput = Index;
		});
		if (InvalidInput) return std::unexpected(RejectCommand("Create this node from a compatible texture or Surface output."));
		const auto Id = Expression->Id;
		State.Presentation.Nodes.push_back({Id, X, Y, DisplayName});
		State.Expressions.emplace_back(Expression.Get());
		auto Result = State.Commit("Create Graph Expression", Transactions);
		if (!Result) return std::unexpected(std::move(Result));
		auto Primary = GetCreatedNode(Id);
		if (Primary && AuxiliaryNodes && GeneratedStaticBoolId.IsValid())
		{
			auto Condition = GetCreatedNode(GeneratedStaticBoolId);
			if (!Condition) return Condition;
			AuxiliaryNodes->push_back(*Condition);
		}
		return Primary;
	}

	auto DMaterialGraph::CreateExpression(const DMaterialExpression& Expression, int32 X, int32 Y,
		ETransactionMode Mode) -> TMaterialGraphResult<DMaterialGraphNode*>
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return std::unexpected(RejectCommand(Admission.error()));
		auto* Transactions = *Admission;
		if (!Owner.IsValid()) return std::unexpected(RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner));
		if (!GetSchema().CanCreateExpression(Expression)) return std::unexpected(RejectCommand("This expression is not supported by the graph."));
		FGraphEditSession State(*Owner.Get());
		if (std::ranges::any_of(State.Expressions, [&](const auto& Value) { return Value->Id == Expression.Id; }))
			return std::unexpected(RejectCommand("The expression GUID already exists."));
		const auto Duplicated = DuplicateObject(&Expression, nullptr, NAME_None);
		if (!Duplicated) return std::unexpected(RejectCommand(ToString(Duplicated.error())));
		auto* Copy = *Duplicated;
		State.Expressions.emplace_back(Copy);
		if (!Copy->Id.IsValid()) Copy->Id = FGuid::NewGuid();
		const auto Id = Copy->Id;
		if (auto* Parameter = Cast<DMaterialExpressionParameter>(Copy))
		{
			if (const auto Error = ResolveParameterExpression(State, *Parameter); !Error) return std::unexpected(Error);
		}
		State.Presentation.Nodes.push_back({Id, X, Y});
		auto Result = State.Commit("Create Graph Expression", Transactions);
		if (!Result) return std::unexpected(std::move(Result));
		return GetCreatedNode(Id);
	}

	auto DMaterialGraph::ReplaceExpression(const DMaterialExpression& Expression,
		ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		if (!GetSchema().CanCreateExpression(Expression)) return RejectCommand("This expression is not supported by the graph.");
		const auto Duplicated = DuplicateObject(&Expression, nullptr, NAME_None);
		if (!Duplicated) return RejectCommand(ToString(Duplicated.error()));
		TStrongObjectPtr<DMaterialExpression> Replacement(Duplicated.value());
		return CommitExpressionReplacement(*Owner.Get(), *Replacement, Transactions);
	}

	auto GraphEditInternals::CommitExpressionReplacement(DObject& Owner, DMaterialExpression& Replacement,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FGraphEditSession State(Owner);
		const auto Id = Replacement.Id;
		const auto Existing = std::ranges::find_if(State.Expressions, [&](const auto& E) { return E->Id == Id; });
		if (Existing == State.Expressions.end()) return RejectCommand("The expression no longer exists.");
		if (auto* Parameter = Cast<DMaterialExpressionParameter>(&Replacement))
			if (const auto Error = ResolveParameterExpression(State, *Parameter,
				Cast<DMaterialExpressionParameter>(Existing->Get())); !Error) return Error;
		if ((*Existing)->GetClass() == Replacement.GetClass())
		{
			if (const auto Assigned = State.Assign(**Existing, Replacement); !Assigned) return Assigned;
		}
		else State.ReplaceExpression(*Existing, Replacement);
		return State.Commit("Replace Graph Expression", Transactions);
	}

	auto DMaterialGraph::InsertFunctionCall(DMaterialFunctionInterface& Function,
		int32 X, int32 Y, ETransactionMode Mode) -> TMaterialGraphResult<DMaterialGraphNode*>
	{
		return InsertFunctionCall(Function, X, Y, {}, Mode);
	}

	auto DMaterialGraph::InsertFunctionCall(DMaterialFunctionInterface& Function,
		int32 X, int32 Y, std::span<const FMaterialFunctionInputBinding> Inputs,
		ETransactionMode Mode) -> TMaterialGraphResult<DMaterialGraphNode*>
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return std::unexpected(RejectCommand(Admission.error()));
		auto* Transactions = *Admission;
		if (!Owner.IsValid()) return std::unexpected(RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner));
		FGraphEditSession State(*Owner.Get());
		std::vector<FMaterialFunctionOwnerStamp> Closure;
		const std::array<DMaterialFunctionInterface*, 1> Roots{&Function};
		auto Validation = ValidateMaterialFunctionDependencies(Roots, Closure, EMaterialFunctionValidationMode::Editing);
		if (!Validation) return std::unexpected(RejectCommand("The selected function dependency is invalid.", std::move(Validation.Diagnostics)));
		if (std::ranges::any_of(Closure, [&](const auto& Dependency) { return Dependency.AssetPath == Owner.Get()->GetObjectPath(); }))
			return std::unexpected(RejectCommand("This call would introduce recursive function dependencies."));
		const FGuid Id = FGuid::NewGuid();
		TStrongObjectPtr<DMaterialExpressionFunctionCall> Call(NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None));
		Call->Id = Id;
		if (!Call->SetMaterialFunction(&Function)) return std::unexpected(RejectCommand("The selected function is unavailable."));
		std::unordered_set<FGuid> BoundInputs;
		for (const auto& Input : Inputs)
		{
			auto* Source = FindMaterialExpression(FMaterialExpressionEditing::GetExpressions(*Owner.Get()), Input.Source.SourceNodeId);
			if (Input.Source.SourceNodeId.IsValid() && !Source) return std::unexpected(RejectCommand("The source expression no longer exists."));
			State.IncludeCallOutput(Source, Input.Source.SourceOutputId);
			FMaterialExpressionFunctionInputBinding Binding{Input.InputId, Input.ExpectedType,
				{Source, Input.Source.SourceOutputIndex, Input.Source.SourceOutputId}};
			if (Input.Default.Kind != EMaterialInputDefaultKind::None)
			{
				const auto Count = static_cast<size_t>(Input.Default.Type) + 1;
				const std::array Values{Input.Default.Literal.X, Input.Default.Literal.Y, Input.Default.Literal.Z, Input.Default.Literal.W};
				if (Count > Values.size()) return std::unexpected(RejectCommand("The function input default has an invalid numeric width."));
				Binding.InputDefault.assign(Values.begin(), Values.begin() + Count);
			}
			const auto Slot = std::ranges::find(Call->FunctionInputs, Input.InputId, &FMaterialExpressionFunctionInputBinding::InputId);
			if (Slot == Call->FunctionInputs.end() || !BoundInputs.insert(Input.InputId).second)
				return std::unexpected(RejectCommand("The function input is missing or bound more than once."));
			*Slot = std::move(Binding);
		}
		Validation = ValidateMaterialFunctionCallSignature(*Call.Get(), Function.GetFunctionSignature(), EMaterialFunctionValidationMode::Editing);
		if (!Validation) return std::unexpected(RejectCommand("The function call bindings are invalid.", std::move(Validation.Diagnostics)));
		State.Expressions.emplace_back(Call.Get());
		State.Presentation.Nodes.push_back({Id, X, Y});
		auto Result = State.Commit("Insert Function Call", Transactions);
		if (!Result) return std::unexpected(std::move(Result));
		return GetCreatedNode(Id);
	}

}
