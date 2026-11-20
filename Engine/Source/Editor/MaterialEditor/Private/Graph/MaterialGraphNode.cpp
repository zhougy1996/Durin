#include "Graph/MaterialGraphCatalog.h"
#include "MaterialGraph.h"
#include "MaterialGraphPinSource.h"
#include "MaterialGraphEditSession.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionVector.h"
#include "Materials/Material.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionFunctionIO.h"
#include "Materials/MaterialExpressionInputs.h"
#include "Materials/MaterialExpressionMaterialAttributes.h"
#include "Materials/MaterialExpressionMaterialOutput.h"
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
		if (!ValidateGraphEdit(*Owner))
		{
			// A repair must be able to restore the originally invalid asset on Undo.
			FGraphEditSession State(*Owner);
			State.Modify(*Expression);
			Expression->SetEditableName(std::move(NewName));
			return State.Commit("Rename Material Expression", *Admission);
		}
		FExpressionEdit Edit;
		return Edit.CommitRename(*Owner, *Expression, std::move(NewName), *Admission);
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
		auto Metadata = *Target;
		Metadata.Name = Port.Name;
		Metadata.DisplayOrder = Port.DisplayOrder;
		// Use the compound path when repairing an already-invalid function signature.
		if (Metadata == Port && ValidateGraphEdit(*Function))
		{
			auto Signature = Function->GetFunctionSignature();
			auto& Ports = Cast<DMaterialExpressionFunctionOutput>(Expression) ? Signature.Outputs : Signature.Inputs;
			const auto It = std::ranges::find(Ports, Port.Id, &FMaterialFunctionPort::Id);
			if (It == Ports.end()) return RejectCommand("The function port is unavailable.");
			*It = Port;
			if (const auto Valid = ValidateMaterialFunctionSignature(Signature); !Valid)
				return RejectCommand("The function signature is invalid.", Valid.Diagnostics);
			FExpressionEdit Edit;
			Edit.Set(*Expression, *Target, std::move(Port));
			return Edit.Commit(*Function, *Admission, "Edit Function Port");
		}
		FGraphEditSession State(*Function);
		State.Modify(*Expression);
		*Target = std::move(Port);
		return State.Commit("Edit Function Port", *Admission);
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
				FExpressionEdit Edit;
				Edit.Set(*Constant, Constant->Value, NewValue);
				return Edit.Commit(*Owner, *Admission, "Edit Constant Expression");
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
		FExpressionEdit Edit;
		Edit.Set(*Swizzle, Swizzle->Components, std::vector<uint8>(Components.begin(), Components.end()));
		return Edit.CommitInferred(*Owner, *Admission, "Edit Swizzle",
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
		FExpressionEdit Edit;
		auto Outputs = Surface->AttributeIDs;
		if (Enabled) Outputs.push_back(AttributeId);
		else std::erase(Outputs, AttributeId);
		Edit.Set(*Surface, Surface->AttributeIDs, std::move(Outputs));
		if (!Enabled)
		{
			const auto UsesOutput = [&](const FMaterialExpressionInput& Input) {
				return Input.Expression.Get() == Surface && Input.OutputIndex == static_cast<int32>(Definition->Property);
			};
			// Hidden root inputs also retain authored connections.
			for (const auto& Consumer : FMaterialExpressionEditing::GetExpressions(Owner))
			{
				VisitStoredMaterialInputs(*Consumer, [&](FMaterialExpressionInput& Input) {
					if (UsesOutput(Input)) Edit.SetLink(*Consumer, Input, {});
				});
			}
		}
		return Edit.CommitInferred(Owner, *Admission, "Edit Material Attribute Outputs");
	}

	auto DMaterialGraphNode::Refresh(FMaterialGraphNodeView Metadata, FMaterialGraphPinSource Source, FPinOperation Operation) -> void
	{
		MaterialExpression = Source.Expression;
		View = std::move(Metadata);
		SetExpressionLabels(View, Source.Expression);
		struct FRestoreSource
		{
			const FMaterialGraphPinSource*& Slot;
			const FMaterialGraphPinSource* Previous;
			~FRestoreSource() { Slot = Previous; }
		} Restore{ActiveSource, std::exchange(ActiveSource, &Source)};
		(this->*Operation)();
	}

	auto DMaterialGraphNode::CreatePins() -> void
	{
		View.Inputs.clear();
		View.Outputs.clear();
		CreateInputPins();
		CreateOutputPins();

		Pins.clear();
		Pins.reserve(View.Inputs.size() + View.Outputs.size());
		const auto* Call = Cast<DMaterialExpressionFunctionCall>(ActiveSource->Expression);
		for (const auto& Input : View.Inputs)
		{
			auto Index = static_cast<int32>(Input.InputIndex);
			if (Call)
			{
				const auto It = std::ranges::find(Call->FunctionInputs, Input.PortId, &FMaterialExpressionFunctionInputBinding::InputId);
				Index = It == Call->FunctionInputs.end() ? -1 : static_cast<int32>(It - Call->FunctionInputs.begin());
			}
			Pins.push_back(FMaterialGraphPin(this, EMaterialGraphPinDirection::Input, Index, static_cast<int32>(&Input - View.Inputs.data())));
		}
		for (const auto& Output : View.Outputs)
			Pins.push_back(FMaterialGraphPin(this, EMaterialGraphPinDirection::Output, Output.SourceIndex, static_cast<int32>(&Output - View.Outputs.data())));
	}

	auto DMaterialGraphNode::GetInputPin(int32 Index) const -> const FMaterialGraphPin*
	{
		return Index >= 0 && static_cast<size_t>(Index) < View.Inputs.size() ? &Pins[Index] : nullptr;
	}

	auto DMaterialGraphNode::GetOutputPin(int32 Index) const -> const FMaterialGraphPin*
	{
		return Index >= 0 && static_cast<size_t>(Index) < View.Outputs.size() ? &Pins[View.Inputs.size() + Index] : nullptr;
	}

	auto DMaterialGraphNode::AllocateDefaultPins() -> void
	{
		if (ActiveSource)
		{
			CreatePins();
			return;
		}
		if (!GetMaterialExpression()) return;
		auto* Graph = Cast<DMaterialGraph>(GetOuter());
		Graph->RefreshNodes(EnumerateCatalog(), std::span(&View.Node.Id, 1));
	}

	auto DMaterialGraphNode::ReconstructNode() -> void { AllocateDefaultPins(); }


	auto DMaterialGraphNode_Root::CreateInputPins() -> void
	{
		const auto& Source = *ActiveSource;
		const auto* Material = Source.Material;
		auto* Terminal = Cast<DMaterialExpressionMaterialOutput>(Source.Expression);
		if (!Material || !Terminal) return;
		for (const auto& Definition : GetMaterialDomainOutputPins(Material->GetDomain()))
		{
			const bool bAttributes = Definition.Id == EMaterialOutputPin::Surface;
			if (bAttributes != Terminal->Outputs.bUseMaterialAttributes) continue;
			FMaterialGraphPinView Pin{.InputIndex = static_cast<uint32>(Definition.Id), .Name = std::string(Definition.Name),
				.AcceptedTypes = {Definition.Type}};
			if (Definition.Id == EMaterialOutputPin::WorldPositionOffset)
				Pin.Constraint.Stages = EMaterialEvaluationStage::Vertex;
			else if (!bAttributes)
			{
				const auto Semantics = GetMaterialSurfaceOutputSemantics(
					static_cast<EMaterialProperty>(Definition.Id));
				Pin.Constraint = {.Mode = EMaterialFunctionValueConstraintMode::Exact,
					.Stages = Semantics.Stages, .Kind = Semantics.Kind,
					.Space = Semantics.Space};
			}
			if (Definition.Id != EMaterialOutputPin::WorldPositionOffset && Definition.Id != EMaterialOutputPin::Normal
				&& Definition.Type > EMaterialProgramValueType::Float && Definition.Type <= EMaterialProgramValueType::Float4)
				Pin.AcceptedTypes.push_back(EMaterialProgramValueType::Float);
			Pin.InlineDefault = DefaultView(ReadMaterialOutputDefault(Terminal->Outputs, Definition.Id));
			if (const auto* Numeric = GetMaterialOutputNumericInput(Terminal->Outputs, Definition.Id))
			{
				Pin.bSupportsConstant = true; Pin.bUseConstant = Numeric->UseConstant;
				Pin.RetainedConstant = DefaultView(Numeric->Constant);
			}
			Pin.bActive = bAttributes || Definition.Id == EMaterialOutputPin::WorldPositionOffset || IsMaterialSurfaceOutputActive(
				static_cast<EMaterialProperty>(Definition.Id), Material->GetStaticProperties());
			View.Inputs.push_back(std::move(Pin));
		}
	}

	auto DMaterialGraphNode::CreateInputPins() -> void
	{
		const auto& Source = *ActiveSource;
		auto* Expression = Source.Expression;
		const auto& Node = View.Node;
		const auto* Shape = Source.Shape;
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
				}
		}
		if (const auto* Output = Cast<DMaterialExpressionFunctionOutput>(Expression); Output && !View.Inputs.empty())
		{
			View.Inputs.front().Name = Output->Port.Name;
			View.Inputs.front().AcceptedTypes = {Output->Port.Type};
		}
		if ((Node.Opcode == EMaterialProgramOpcode::GetMaterialAttributes
			|| Node.Opcode == EMaterialProgramOpcode::SetMaterialAttributes) && !View.Inputs.empty())
		{
			View.Inputs[0].Name = "Surface";
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
					.AcceptedTypes = {GetMaterialSurfaceOutputType(Property)},
					.InlineDefault = Attribute.Source.UseConstant ? DefaultView(Attribute.Source.Constant) : FMaterialInputDefault{},
					.RetainedConstant = DefaultView(Attribute.Source.Constant), .bSupportsConstant = true,
					.bUseConstant = Attribute.Source.UseConstant});
			}
		for (auto& Pin : View.Inputs)
			if (Node.Opcode != EMaterialProgramOpcode::Normalize && Node.Opcode != EMaterialProgramOpcode::SceneTexture && Node.Opcode != EMaterialProgramOpcode::SceneDepth && !IsMaterialSamplingNode(Node.Opcode) && Pin.AcceptedTypes.size() == 1
				&& Pin.AcceptedTypes.front() != EMaterialProgramValueType::Float
				&& FMaterialGraphSchema::AcceptsPort(Pin.AcceptedTypes.front(), EMaterialProgramValueType::Float))
				Pin.AcceptedTypes.push_back(EMaterialProgramValueType::Float);
	}

	auto DMaterialGraphNode::CreateOutputPins() -> void
	{
		const auto& Source = *ActiveSource;
		auto* Expression = Source.Expression;
		const auto& Node = View.Node;
		if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression))
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
		else if (Node.Opcode != EMaterialProgramOpcode::FunctionOutput)
			View.Outputs.push_back({.Name = GetProgramTypeName(Node.ResultType), .Type = Node.ResultType});
		if (const auto* Input = Cast<DMaterialExpressionFunctionInput>(Expression); Input && !View.Outputs.empty())
			View.Outputs.front().Name = Input->Port.Name;
	}

}
