#include "Graph/MaterialGraphCatalog.h"
#include "MaterialGraph.h"
#include "MaterialGraphCommands.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionVector.h"
#include "Materials/Material.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpressionCustomOutput.h"
#include "Materials/MaterialExpressionDescription.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionFunctionIO.h"
#include "Materials/MaterialExpressionInputs.h"
#include "Materials/MaterialExpressionMaterialAttributes.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureParameter.h"
#include "Graph/MaterialGraphValueTypes.h"

namespace Durin::MaterialEditor
{
	namespace
	{
		auto GetTerminalPort(DMaterialExpression* Expression) -> FMaterialFunctionPort*
		{
			if (auto* Input = Cast<DMaterialExpressionFunctionInput>(Expression)) return &Input->Port;
			if (auto* Output = Cast<DMaterialExpressionFunctionOutput>(Expression)) return &Output->Port;
			return nullptr;
		}
		auto SetExpressionLabels(FMaterialGraphNodeView& View, DMaterialExpression* Expression) -> void
		{
			if (View.Node.bMaterialOutput) return;
			if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression))
			{
				if (!Call->Function.IsValid()) { View.SecondaryLabel = "Missing function"; return; }
				View.FunctionPath = Call->Function->GetObjectPath();
				View.SecondaryLabel = View.PrimaryLabel;
				View.PrimaryLabel = Call->Function->GetName();
				return;
			}
			if (const auto* Port = GetTerminalPort(Expression))
			{
				View.SecondaryLabel = std::format("{} ({})", View.PrimaryLabel, GetProgramTypeName(Port->Type));
				if (!Port->Name.empty()) View.PrimaryLabel = Port->Name;
				return;
			}
			if (const auto* Parameter = Cast<DMaterialExpressionParameter>(Expression))
			{
				View.SecondaryLabel = View.PrimaryLabel;
				View.PrimaryLabel = Parameter->Metadata.DisplayName.empty() ? Parameter->Metadata.Name.ToString() : Parameter->Metadata.DisplayName;
			}
			if (const auto* Swizzle = Cast<DMaterialExpressionSwizzle>(Expression))
			{
				View.PrimaryLabel += " ";
				for (const uint8 Component : Swizzle->Components)
					View.PrimaryLabel += Component < 4 ? std::string(1, "RGBA"[Component]) : "?";
			}
			if (!View.Node.GetParameterId().IsValid()) View.SecondaryLabel = View.Presentation.DisplayName;
		}

		auto DefaultView(std::span<const float> Values) -> FMaterialInputDefault {
			if (Values.empty() || Values.size() > 4) return {};
			FMaterialInputDefault Result{.Kind = EMaterialInputDefaultKind::Literal,
				.Type = static_cast<EMaterialProgramValueType>(Values.size() - 1)};
			const std::array Lanes{&Result.Literal.X, &Result.Literal.Y, &Result.Literal.Z, &Result.Literal.W};
			for (size_t Index = 0; Index < Values.size(); ++Index) *Lanes[Index] = Values[Index];
			return Result;
		}
		auto OrderedPorts(auto Ports)
		{
			std::ranges::stable_sort(Ports, {}, &FMaterialFunctionPort::DisplayOrder);
			return Ports;
		}
	}

	auto DMaterialGraphNode::OnRenameNode(std::string NewName, ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		using namespace GraphEditInternals;
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Graph = Cast<DMaterialGraph>(GetOuter());
		auto* Owner = Graph ? Graph->GetMaterialOrFunction() : nullptr;
		auto* Expression = GetMaterialExpression();
		if (!Owner || !Expression || !Expression->CanRenameNode()) return RejectCommand("The selected node cannot be renamed.");
		if (Expression->GetEditableName() == NewName) return {.Status = EMaterialGraphCommandStatus::NoChange};
		if (const auto* Terminal = GetTerminalPort(Expression))
		{
			auto Port = *Terminal;
			Port.Name = std::move(NewName);
			return SetFunctionPort(std::move(Port), Mode);
		}
		const auto Duplicated = DuplicateObject(Expression, nullptr, NAME_None);
		if (!Duplicated) return RejectCommand(ToString(Duplicated.error()));
		TStrongObjectPtr<DMaterialExpression> Draft(Cast<DMaterialExpression>(*Duplicated));
		Draft->SetEditableName(std::move(NewName));
		return CommitExpressionReplacement(*Owner, *Draft, *Admission);
	}

	auto DMaterialGraphNode::SetFunctionPort(FMaterialFunctionPort Port, ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		using namespace GraphEditInternals;
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Graph = Cast<DMaterialGraph>(GetOuter());
		auto* Function = Graph ? Cast<DMaterialFunction>(Graph->GetMaterialOrFunction()) : nullptr;
		auto* Expression = GetMaterialExpression();
		auto* Target = GetTerminalPort(Expression);
		if (!Function || !Target || Target->Id != Port.Id) return RejectCommand("The function port is unavailable.");
		if (*Target == Port) return {.Status = EMaterialGraphCommandStatus::NoChange};
		const auto Duplicated = DuplicateObject(Expression, nullptr, NAME_None);
		if (!Duplicated) return RejectCommand(ToString(Duplicated.error()));
		TStrongObjectPtr<DMaterialExpression> Draft(Cast<DMaterialExpression>(*Duplicated));
		*GetTerminalPort(Draft.Get()) = std::move(Port);
		return CommitExpressionReplacement(*Function, *Draft, *Admission);
	}

	auto DMaterialGraphNode::SetConstantValue(const FMaterialParameterValue& Value, ETransactionMode Mode)
		-> FMaterialGraphCommandResult
	{
		using namespace GraphEditInternals;
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Graph = Cast<DMaterialGraph>(GetOuter());
		auto* Owner = Graph ? Graph->GetMaterialOrFunction() : nullptr;
		if (!Owner) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		auto* Expression = GetMaterialExpression();
		if (!Expression) return RejectCommand("The expression no longer exists.");
		if (!Cast<DMaterialExpressionScalarConstant>(Expression) && !Cast<DMaterialExpressionVector2Constant>(Expression)
			&& !Cast<DMaterialExpressionVector3Constant>(Expression) && !Cast<DMaterialExpressionVector4Constant>(Expression))
			return RejectCommand("The selected expression is not a constant.");
		const auto Literal = ReadParameterLiteral(GetProgramType(Value.GetType()), Value);
		if (!std::isfinite(Literal.X) || !std::isfinite(Literal.Y) || !std::isfinite(Literal.Z) || !std::isfinite(Literal.W))
			return RejectCommand("Constant values must be finite.", {{.NodeId = Expression->Id, .Message = "Expression constant must be finite."}});

		const auto SetValue = [&]<class T>(const auto& NewValue) -> FMaterialGraphCommandResult {
			if (auto* Constant = Cast<T>(Expression))
			{
				if (Constant->Value == NewValue) return {.Status = EMaterialGraphCommandStatus::NoChange};
				if (auto Valid = ValidateGraphEdit(*Owner); !Valid) return Valid;
				return EditExpressions(*Owner, *Admission, "Edit Constant Expression", std::array{Expression},
					[&] { Constant->Value = NewValue; });
			}
			TStrongObjectPtr<T> Replacement(NewObject<T>(nullptr, NAME_None));
			Replacement->Id = Expression->Id;
			Replacement->Value = NewValue;
			return CommitExpressionReplacement(*Owner, *Replacement, *Admission);
		};
		switch (Value.GetType())
		{
		case EMaterialParameterType::Scalar: return SetValue.operator()<DMaterialExpressionScalarConstant>(Value.GetScalar());
		case EMaterialParameterType::Vector2: return SetValue.operator()<DMaterialExpressionVector2Constant>(Value.GetVector2());
		case EMaterialParameterType::Vector: return SetValue.operator()<DMaterialExpressionVector3Constant>(Value.GetVector());
		case EMaterialParameterType::Vector4: return SetValue.operator()<DMaterialExpressionVector4Constant>(Value.GetVector4());
		default: return RejectCommand("Constants require a numeric value.");
		}
	}

	auto DMaterialGraphNode::SetSwizzleComponents(std::span<const uint8> Components, ETransactionMode Mode)
		-> FMaterialGraphCommandResult
	{
		using namespace GraphEditInternals;
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		if (Components.empty() || Components.size() > 4 || std::ranges::any_of(Components, [](uint8 Channel) { return Channel > 3; }))
			return RejectCommand("Swizzles require one to four valid components.");
		auto* Graph = Cast<DMaterialGraph>(GetOuter());
		auto* Owner = Graph ? Graph->GetMaterialOrFunction() : nullptr;
		if (!Owner) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		auto* Expression = GetMaterialExpression();
		if (!Expression) return RejectCommand("The expression no longer exists.");
		auto* Swizzle = Cast<DMaterialExpressionSwizzle>(Expression);
		if (!Swizzle) return RejectCommand("The selected expression is not a swizzle.");
		if (std::ranges::equal(Swizzle->Components, Components)) return {.Status = EMaterialGraphCommandStatus::NoChange};
		return EditExpressionInputs(*Owner, *Admission, "Edit Swizzle", {Swizzle},
			[&] { Swizzle->Components.assign(Components.begin(), Components.end()); }, {}, {},
			{{Swizzle, static_cast<EMaterialProgramValueType>(Components.size() - 1)}});
	}

	auto DMaterialGraphNode::SetAttributeOutputEnabled(FGuid AttributeId, bool Enabled, ETransactionMode Mode)
		-> FMaterialGraphCommandResult
	{
		using namespace GraphEditInternals;
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Surface = Cast<DMaterialExpressionGetMaterialAttributes>(GetMaterialExpression());
		if (!Surface) return RejectCommand("The selected node has no material attribute outputs.");
		const auto* Definition = FMaterialAttributeDefinitionMap::Find(AttributeId);
		if (!Definition) return RejectCommand("Unknown material attribute.");
		if (Surface->IsSelected(Definition->Property) == Enabled) return {.Status = EMaterialGraphCommandStatus::NoChange};

		auto& Owner = *Surface->GetOuter();
		std::vector<DMaterialExpression*> Objects{Surface};
		std::vector<FMaterialExpressionInput*> Removed;
		FProposedLinks Links;
		auto Outputs = Surface->AttributeIDs;
		if (Enabled) Outputs.push_back(AttributeId);
		else std::erase(Outputs, AttributeId);
		if (!Enabled)
		{
			const auto UsesOutput = [&](const FMaterialExpressionInput& Input) {
				return Input.Expression.Get() == Surface && Input.OutputIndex == static_cast<int32>(Definition->Property);
			};
			// Hidden root inputs also retain authored connections.
			for (const auto& Consumer : FMaterialExpressionEditing::GetExpressions(Owner))
			{
				VisitStoredMaterialInputs(*Consumer, [&](FMaterialExpressionInput& Input) {
					if (UsesOutput(Input))
					{
						if (!std::ranges::contains(Objects, Consumer.Get())) Objects.push_back(Consumer.Get());
						Removed.push_back(&Input);
						Links.emplace(&Input, FMaterialExpressionInput{});
					}
				});
			}
		}
		return EditExpressionInputs(Owner, *Admission, "Edit Material Attribute Outputs", std::move(Objects), [&] {
			Surface->AttributeIDs = std::move(Outputs);
			for (auto* Input : Removed) *Input = {};
		}, Links);
	}

	auto DMaterialGraphNode::Refresh(DMaterialExpression* Expression, size_t Ordinal) -> void
	{
		if (MaterialExpression.Get() != Expression) Pins.clear();
		MaterialExpression = Expression;
		FMaterialGraphNodeDescriptor Node{.Id = Expression->Id};
		const auto* Description = FindMaterialExpressionDescription(Expression->GetClass());
		if (Description)
		{
			if (Description->SemanticOpcode) Node.Opcode = *Description->SemanticOpcode;
			if (!Description->Shapes.empty()) Node.ResultType = Description->Shapes.front().ResultType;
		}
		if (const auto* Texture = Cast<DMaterialExpressionTextureParameter>(Expression); Texture && !IsMaterialSamplingNode(Node.Opcode))
			Node.ResultType = Texture->DefaultValue.Dimension == EMaterialTextureDimension::TextureCube ? EMaterialProgramValueType::TextureCube : EMaterialProgramValueType::Texture2D;
		if (auto* Type = Expression->GetClass()->FindPropertyByName("ResultType"))
			Node.ResultType = *static_cast<const EMaterialProgramValueType*>(Type->GetValuePtr(Expression));
		if (const auto* Constant = Cast<DMaterialExpressionScalarConstant>(Expression)) Node.Data = FMaterialParameterValue::MakeScalar(Constant->Value);
		else if (const auto* Constant = Cast<DMaterialExpressionVector2Constant>(Expression)) Node.Data = FMaterialParameterValue::MakeVector2(Constant->Value);
		else if (const auto* Constant = Cast<DMaterialExpressionVector3Constant>(Expression)) Node.Data = FMaterialParameterValue::MakeVector(Constant->Value);
		else if (const auto* Constant = Cast<DMaterialExpressionVector4Constant>(Expression)) Node.Data = FMaterialParameterValue::MakeVector4(Constant->Value);
		else if (const auto* Sample = Cast<DMaterialExpressionTextureSampleParameterCube>(Expression)) Node.Data = FMaterialGraphSampleInfo{Sample->Metadata.Id};
		else if (Cast<DMaterialExpressionTextureSampleCube>(Expression)) Node.Data = FMaterialGraphSampleInfo{};
		else if (const auto* Sample = Cast<DMaterialExpressionTextureSampleParameter2D>(Expression)) Node.Data = FMaterialGraphSampleInfo{Sample->Metadata.Id};
		else if (const auto* Sample = Cast<DMaterialExpressionTextureSample2D>(Expression)) Node.Data = FMaterialGraphSampleInfo{};
		else if (const auto* Parameter = Cast<DMaterialExpressionParameter>(Expression)) Node.Data = FMaterialGraphParameterInfo{Parameter->Metadata.Id};
		else if (const auto* Swizzle = Cast<DMaterialExpressionSwizzle>(Expression))
		{
			Node.Data = Swizzle->Components;
			if (!Swizzle->Components.empty()) Node.ResultType = static_cast<EMaterialProgramValueType>(Swizzle->Components.size() - 1);
		}
		if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression))
		{
			Node.Opcode = EMaterialProgramOpcode::FunctionCall;
			if (!Call->FunctionOutputs.empty()) Node.ResultType = Call->FunctionOutputs.front().ExpectedType;
		}
		if (const auto* Collection = Cast<DMaterialExpressionCollectionParameter>(Expression); Collection && IsValid(Collection->Collection.Get()))
			if (const auto* Declaration = Collection->Collection->FindDeclaration(Collection->ParameterId))
				Node.ResultType = GetProgramType(Declaration->Type);
		const auto* Input = Cast<DMaterialExpressionFunctionInput>(Expression);
		const auto* Output = Cast<DMaterialExpressionFunctionOutput>(Expression);
		if (Input || Output)
		{
			Node.Opcode = Input ? EMaterialProgramOpcode::FunctionInput : EMaterialProgramOpcode::FunctionOutput;
			Node.ResultType = Input ? Input->Port.Type : Output->Port.Type;
		}
		const FMaterialExpressionAuthoringShape* Shape = nullptr;
		if (Description)
		{
			const auto It = std::ranges::find(Description->Shapes, Node.ResultType, &FMaterialExpressionAuthoringShape::ResultType);
			if (It != Description->Shapes.end()) Shape = &*It;
		}
		const auto& Layout = Expression->EditorLayout;
		auto Label = Description ? std::string(Description->Name) : std::string(Expression->GetClass()->GetDisplayName());
		if (Shape && Node.Opcode == EMaterialProgramOpcode::Parameter)
			Label = Node.ResultType == EMaterialProgramValueType::Float ? "Scalar Parameter" : "Vector Parameter";
		View = {.Node = std::move(Node), .PrimaryLabel = std::move(Label)};
		View.Presentation = Layout.bAuthored
			? FMaterialGraphNodePresentation{View.Node.Id, Layout.X, Layout.Y, Layout.DisplayName}
			: DefaultNodePresentation(View.Node, Ordinal);
		SetExpressionLabels(View, Expression);
		CreatePins(Shape);
	}

	auto DMaterialGraphNode_Root::Refresh(const DMaterial& Material, FGuid Id) -> void
	{
		const auto [X, Y] = Material.GetRootPosition();
		View = {.Node = {.Id = Id, .bMaterialOutput = true}, .PrimaryLabel = "Material Output",
			.Presentation = {Id, X, Y}};
		CreatePins(nullptr);
	}

	auto DMaterialGraphNode::CreatePins(const FMaterialExpressionAuthoringShape* Shape) -> void
	{
		CreateInputPins(Shape);
		CreateOutputPins();

		const auto Previous = std::move(Pins);
		Pins.clear();
		Pins.reserve(View.Inputs.size() + View.Outputs.size());
		const auto AddPin = [&](EMaterialGraphPinDirection Direction, int32 Index, int32 Ordinal, FGuid Port) {
			auto Pin = FMaterialGraphPin(this, Direction, Index, Ordinal, Port);
			const auto Match = std::ranges::find_if(Previous, [&](const FMaterialGraphPin& Old) {
				return Old.Direction == Direction && Old.PortId == Port && (Port.IsValid() || Old.SourceIndex == Index);
			});
			Pin.Id = Match == Previous.end() ? FGuid::NewGuid() : Match->Id;
			Pins.push_back(std::move(Pin));
		};
		const auto* Call = Cast<DMaterialExpressionFunctionCall>(MaterialExpression.Get());
		for (const auto& Input : View.Inputs)
		{
			auto Index = static_cast<int32>(Input.InputIndex);
			if (Call)
			{
				const auto It = std::ranges::find(Call->FunctionInputs, Input.PortId, &FMaterialExpressionFunctionInputBinding::InputId);
				Index = It == Call->FunctionInputs.end() ? -1 : static_cast<int32>(It - Call->FunctionInputs.begin());
			}
			AddPin(EMaterialGraphPinDirection::Input, Index, static_cast<int32>(&Input - View.Inputs.data()), Input.PortId);
		}
		for (const auto& Output : View.Outputs)
			AddPin(EMaterialGraphPinDirection::Output, Output.SourceIndex, static_cast<int32>(&Output - View.Outputs.data()), Output.PortId);
	}

	auto DMaterialGraphNode::GetInputPin(int32 Index) const -> const FMaterialGraphPin*
	{
		return Index >= 0 && static_cast<size_t>(Index) < View.Inputs.size() ? &Pins[Index] : nullptr;
	}

	auto DMaterialGraphNode::GetOutputPin(int32 Index) const -> const FMaterialGraphPin*
	{
		return Index >= 0 && static_cast<size_t>(Index) < View.Outputs.size() ? &Pins[View.Inputs.size() + Index] : nullptr;
	}

	auto DMaterialGraphNode::FindInputPin(uint32 Index) const -> const FMaterialGraphPin*
	{
		const auto It = std::ranges::find_if(View.Inputs, [&](const auto& Pin) { return !Pin.MaterialProperty && !Pin.PortId.IsValid() && Pin.InputIndex == Index; });
		return GetInputPin(static_cast<int32>(It - View.Inputs.begin()));
	}

	auto DMaterialGraphNode::FindInputPin(FGuid Port) const -> const FMaterialGraphPin*
	{
		if (!Port.IsValid()) return nullptr;
		const auto It = std::ranges::find(View.Inputs, Port, &FMaterialGraphPinView::PortId);
		return GetInputPin(static_cast<int32>(It - View.Inputs.begin()));
	}

	auto DMaterialGraphNode::FindInputPin(EMaterialProperty Property) const -> const FMaterialGraphPin*
	{
		const auto It = std::ranges::find(View.Inputs, std::optional{Property}, &FMaterialGraphPinView::MaterialProperty);
		return GetInputPin(static_cast<int32>(It - View.Inputs.begin()));
	}

	auto DMaterialGraphNode::FindPinById(FGuid PinId) const -> const FMaterialGraphPin*
	{
		const auto It = std::ranges::find(Pins, PinId, &FMaterialGraphPin::Id);
		return It == Pins.end() ? nullptr : &*It;
	}

	auto DMaterialGraphNode::AllocateDefaultPins() -> void
	{
		auto* Graph = Cast<DMaterialGraph>(GetOuter());
		if (!Graph || Graph->FindNode(View.Node.Id) != this) return;
		FMaterialGraphChangeSet Changes;
		Changes.MarkNode(View.Node.Id, EMaterialGraphNodeChange::Content);
		Graph->ApplyChanges(std::move(Changes));
	}

	auto DMaterialGraphNode::ReconstructNode() -> void { AllocateDefaultPins(); }


	auto DMaterialGraphNode_Root::CreateInputPins(const FMaterialExpressionAuthoringShape*) -> void
	{
		const auto* Material = Cast<DMaterial>(Cast<DMaterialGraph>(GetOuter())->GetMaterialOrFunction());
		if (!Material) return;
		const auto Properties = GetMaterialDomainInputProperties(EMaterialDomain::Surface);
		const auto Active = FMaterialAttributeDefinitionMap::GetActiveProperties(Material->GetDomain(), Material->GetStaticProperties());
		const bool bPacked = Material->UsesMaterialAttributes();
		for (uint32 Index = 0; Index < Properties.size(); ++Index)
		{
			const auto Property = Properties[Index];
			const bool bAttributes = Property == EMaterialProperty::MP_MaterialAttributes;
			const auto Description = Material->GetExpressionInputDescription(Property);
			check(Description);
			FMaterialGraphPinView Pin{.InputIndex = Index, .MaterialProperty = Property, .Name = std::string(Description->Name),
				.AcceptedTypes = {Description->Type}};
			if (Property == EMaterialProperty::MP_WorldPositionOffset)
				Pin.Constraint.Stages = EMaterialEvaluationStage::Vertex;
			else if (!bAttributes)
			{
				const auto Semantics = GetMaterialSurfaceOutputSemantics(Property);
				Pin.Constraint = {.Mode = EMaterialFunctionValueConstraintMode::Exact,
					.Stages = Semantics.Stages, .Kind = Semantics.Kind,
					.Space = Semantics.Space};
			}
			if (MaterialInputAllowsValidCast(EMaterialProgramOpcode::MakeMaterialAttributes, uint32(Property)))
				AppendMaterialValidCastSources(Pin.AcceptedTypes);
			if (const auto& Numeric = Description->Numeric)
			{
				Pin.InlineDefault = DefaultView(ReadMaterialNumericDefault(*Numeric, Property));
				if (SupportsMaterialRootConstant(Property))
				{
					Pin.bSupportsConstant = true;
					Pin.bUseConstant = Numeric->UseConstant;
					Pin.RetainedConstant = DefaultView(Numeric->Constant);
				}
			}
			Pin.bActive = bPacked ? bAttributes : std::ranges::contains(Active, Property);
			View.Inputs.push_back(std::move(Pin));
		}
	}

	auto DMaterialGraphNode::CreateInputPins(const FMaterialExpressionAuthoringShape* Shape) -> void
	{
		auto* Expression = MaterialExpression.Get();
		const auto& Node = View.Node;
		const auto Definitions = FMaterialAttributeDefinitionMap::GetDefinitions();
		if (!Cast<DMaterialExpressionFunctionCall>(Expression))
			VisitMaterialExpressionInputs(*Expression, [&](uint32 InputIndex, FMaterialExpressionInput& Input) {
				// Surface override indices are stable attributes, not binding-array ordinals.
				if (Cast<DMaterialExpressionSetMaterialAttributes>(Expression) && InputIndex != 0) return;
				FMaterialGraphPinView Pin{.InputIndex = InputIndex,
					.Name = Shape && InputIndex < Shape->InputNames.size() ? Shape->InputNames[InputIndex] : "Value"};
				if (Shape && InputIndex < Shape->AcceptedInputTypes.size()) Pin.AcceptedTypes = Shape->AcceptedInputTypes[InputIndex];
				if (IsMaterialAdaptiveNumeric(Node.Opcode) && !(Node.Opcode == EMaterialProgramOpcode::Lerp && InputIndex == 2))
				{
					// Keep the resolved width first for pin styling, while allowing reconnection.
					Pin.AcceptedTypes = {Node.ResultType};
					for (const auto Type : {EMaterialProgramValueType::Float, EMaterialProgramValueType::Float2,
						EMaterialProgramValueType::Float3, EMaterialProgramValueType::Float4})
						if (Type != Node.ResultType && !(Node.Opcode == EMaterialProgramOpcode::Normalize && Type == EMaterialProgramValueType::Float))
							Pin.AcceptedTypes.push_back(Type);
				}
				if (const auto* Numeric = FindMaterialNumericInput(*Expression, Input))
				{
					Pin.bUseConstant = Numeric->UseConstant;
					Pin.bSupportsConstant = true;
					Pin.RetainedConstant = DefaultView(Numeric->Constant);
					Pin.InlineDefault = DefaultView(Numeric->UseConstant ? Numeric->Constant
						: GetMaterialNumericInputFallback(Node.Opcode, Node.ResultType, InputIndex));
				}
				if (!Input.IsConnected() && Pin.InlineDefault.Kind != EMaterialInputDefaultKind::None) Pin.SourceType = Pin.InlineDefault.Type;
				else if (!Input.IsConnected() && !Pin.AcceptedTypes.empty()) Pin.SourceType = Pin.AcceptedTypes.front();
				const auto SetSamplingActivity = [&](const auto* Sample, uint32 FirstOptional) {
					if (!Sample || InputIndex < FirstOptional) return;
					Pin.bActive = InputIndex == FirstOptional
						? Sample->ReadMode == EMaterialTextureReadMode::Level || Sample->ReadMode == EMaterialTextureReadMode::Bias
						: Sample->ReadMode == EMaterialTextureReadMode::Grad;
				};
				SetSamplingActivity(Cast<DMaterialExpressionTextureSampleCube>(Expression), 2);
				SetSamplingActivity(Cast<DMaterialExpressionTextureSampleParameterCube>(Expression), 1);
				SetSamplingActivity(Cast<DMaterialExpressionTextureSample2D>(Expression), 2);
				SetSamplingActivity(Cast<DMaterialExpressionTextureSampleParameter2D>(Expression), 1);
				View.Inputs.push_back(std::move(Pin));
			});
		if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression))
		{
			if (IsValid(Call->Function.Get()))
				for (const auto& Port : OrderedPorts(Call->Function->GetFunctionSignature().Inputs))
				{
					const auto Binding = std::ranges::find(Call->FunctionInputs, Port.Id, &FMaterialExpressionFunctionInputBinding::InputId);
					View.Inputs.push_back({.InputIndex = static_cast<uint32>(View.Inputs.size()), .Name = Port.Name,
						.SourceType = Port.Type,
						.AcceptedTypes = {Port.Type}, .PortId = Port.Id, .bRequired = Port.bRequired, .Default = Port.Default,
						.Constraint = Port.Constraint,
						.InlineDefault = Binding == Call->FunctionInputs.end() ? FMaterialInputDefault{} : DefaultView(Binding->InputDefault),
						.bAdvanced = Port.bAdvanced});
					AppendMaterialValidCastSources(View.Inputs.back().AcceptedTypes);
				}
		}
		if (const auto* Output = Cast<DMaterialExpressionFunctionOutput>(Expression); Output && !View.Inputs.empty())
		{
			View.Inputs.front().Name = Output->Port.Name;
			View.Inputs.front().AcceptedTypes = {Output->Port.Type};
			AppendMaterialValidCastSources(View.Inputs.front().AcceptedTypes);
		}
		if ((Node.Opcode == EMaterialProgramOpcode::GetMaterialAttributes
			|| Node.Opcode == EMaterialProgramOpcode::SetMaterialAttributes) && !View.Inputs.empty())
		{
			View.Inputs[0].Name = "Material Attributes";
			View.Inputs[0].AcceptedTypes = {EMaterialProgramValueType::MaterialAttributes};
		}
		if (Node.Opcode == EMaterialProgramOpcode::SetMaterialAttributes)
			for (const auto& Attribute : Cast<DMaterialExpressionSetMaterialAttributes>(Expression)->Attributes)
			{
				const auto Property = Attribute.GetProperty();
				const auto Index = static_cast<uint32>(Property);
				if (Index >= Definitions.size()) continue;
				View.Inputs.push_back({.InputIndex = Index + 1, .Name = std::string(Definitions[Index].Name),
					.SourceType = GetMaterialSurfaceOutputType(Property),
					.AcceptedTypes = {GetMaterialSurfaceOutputType(Property)}});
				if (MaterialInputAllowsValidCast(EMaterialProgramOpcode::MakeMaterialAttributes, Index))
					AppendMaterialValidCastSources(View.Inputs.back().AcceptedTypes);
			}
	}

	auto DMaterialGraphNode::CreateOutputPins() -> void
	{
		auto* Expression = MaterialExpression.Get();
		const auto& Node = View.Node;
		if (const auto Outputs = Expression->GetOutputs(); !Outputs.empty())
		{
			for (size_t Index = 0; Index < Outputs.size(); ++Index)
				View.Outputs.push_back({.SourceIndex = static_cast<int32>(Index), .OutputIndex = static_cast<uint8>(Index),
					.Name = std::string(Outputs[Index].Name), .Type = Outputs[Index].Type});
		}
		else if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression))
		{
			std::unordered_map<FGuid, int32> Slots;
			for (int32 Index = 0; Index < static_cast<int32>(Call->FunctionOutputs.size()); ++Index)
				Slots.emplace(Call->FunctionOutputs[Index].OutputId, Index);
			if (IsValid(Call->Function.Get()))
				for (const auto& Port : OrderedPorts(Call->Function->GetFunctionSignature().Outputs))
				{
					const auto Slot = Slots.find(Port.Id);
					View.Outputs.push_back({.SourceIndex = Slot == Slots.end() ? -1 : Slot->second, .PortId = Port.Id,
						.Name = Port.Name, .Type = Port.Type, .Constraint = Port.Constraint});
				}
		}
		else if (IsMaterialSamplingNode(Node.Opcode))
		{
			for (const auto& Output : GetMaterialSampleOutputs(Node.Opcode))
				View.Outputs.push_back({.SourceIndex = static_cast<int32>(Output.Id), .OutputIndex = static_cast<uint8>(Output.Id), .Name = Output.Name, .Type = Output.Type});
		}
		else if (Node.Opcode == EMaterialProgramOpcode::GetMaterialAttributes)
		{
			const auto Definitions = FMaterialAttributeDefinitionMap::GetDefinitions();
			for (uint8 Index = 0; Index < Definitions.size(); ++Index)
				if (Cast<DMaterialExpressionGetMaterialAttributes>(Expression)->IsSelected(Definitions[Index].Property)) View.Outputs.push_back({.SourceIndex = static_cast<int32>(Definitions[Index].Property), .PortId = Definitions[Index].AttributeID,
					.Name = std::string(Definitions[Index].Name), .Type = Definitions[Index].Semantics.Type});
		}
		else if (Node.Opcode != EMaterialProgramOpcode::FunctionOutput && !Cast<DMaterialExpressionCustomOutput>(Expression))
			View.Outputs.push_back({.Name = GetProgramTypeName(Node.ResultType), .Type = Node.ResultType});
		if (const auto* Input = Cast<DMaterialExpressionFunctionInput>(Expression); Input && !View.Outputs.empty())
			View.Outputs.front().Name = Input->Port.Name;
	}

}
