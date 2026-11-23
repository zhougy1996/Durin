#include "MaterialGraph.h"
#include "Graph/MaterialEditingOperations.h"
#include "MaterialExpressionParameters.h"
#include "MaterialGraphValueTypes.h"
#include "Graph/MaterialGraphCatalog.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionVector.h"
#include "Materials/Material.h"
#include "MaterialGraphEditInternals.h"
#include "MaterialGraphCommands.h"
#include "Materials/MaterialExpressionInputs.h"

namespace Durin::MaterialEditor
{
	using namespace GraphEditInternals;

	namespace
	{

		struct FEditableInput
		{
			FMaterialExpressionInput* Source = nullptr;
			std::vector<float>* Default = nullptr;
			std::optional<EMaterialProperty> RootProperty;
			std::string Message;
			FMaterialNumericInput* Numeric = nullptr;
			std::vector<float> Inherited;

			auto SupportsDefault() const -> bool { return Default && (!RootProperty || SupportsMaterialRootConstant(*RootProperty)); }
			auto Read() const -> std::vector<float> { return RootProperty ? ReadMaterialNumericDefault(*Numeric, *RootProperty) : Numeric ? (Numeric->UseConstant ? Numeric->Constant : Inherited) : Default ? *Default : std::vector<float>{}; }
			auto Write(const std::vector<float>& Value) const -> bool
			{
				if (RootProperty && !Value.empty() && Value.size() != Numeric->Constant.size()) return false;
				if (!Default) return false;
				if (Numeric)
				{
					if (Value.empty()) Numeric->UseConstant = false;
					else Numeric->SetConstant(Value);
					return true;
				}
				*Default = Value;
				return true;
			}
		};

		auto IsExpressionMember(DObject& Owner, const DMaterialExpression* Expression) -> bool
		{
			return Expression && std::ranges::contains(FMaterialExpressionEditing::GetExpressions(Owner), Expression,
				[](const auto& Member) { return Member.Get(); });
		}

		auto FindRootInput(FMaterialRootInputs& Root, EMaterialProperty Property) -> FEditableInput
		{
			if (Property == EMaterialProperty::MP_MaterialAttributes) return {.Source = &Root.Attributes};
			auto* Numeric = Root.Properties.Find(Property);
			if (!Numeric) return {.Message = "The material property is unavailable."};
			return {.Source = &Numeric->Connection, .Default = &Numeric->Constant, .RootProperty = Property, .Numeric = Numeric};
		}

		auto FindInput(DMaterialExpression* Expression, FExpressionInputKey Key,
			std::vector<FMaterialExpressionFunctionInputBinding>* DraftBindings = nullptr) -> FEditableInput
		{
			if (!Expression) return {.Message = "The input owner is unavailable."};
			if (const auto* PortId = std::get_if<FGuid>(&Key))
			{
				auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression);
				if (!Call || !Call->Function.IsValid()) return {.Message = "The function input owner is unavailable."};
				const auto& Ports = Call->Function->GetFunctionSignature().Inputs;
				const auto Port = std::ranges::find(Ports, *PortId, &FMaterialFunctionPort::Id);
				if (Port == Ports.end()) return {.Message = "The function input port is unavailable."};
				if (Port->Type > EMaterialProgramValueType::Float4)
				{
					return {.Message = "The function input port is not numeric."};
				}
				auto& Bindings = DraftBindings ? *DraftBindings : Call->FunctionInputs;
				auto Input = std::ranges::find(Bindings, *PortId, &FMaterialExpressionFunctionInputBinding::InputId);
				if (Input == Bindings.end())
				{
					if (!DraftBindings) return {.Message = "The function input has no binding."};
					Input = Bindings.emplace(Bindings.end(), *PortId, Port->Type);
				}
				return {&Input->Input, &Input->InputDefault};
			}
			const auto Index = std::get<uint32>(Key);
			FEditableInput Result;
			VisitMaterialExpressionInputs(*Expression, [&](uint32 Pin, FMaterialExpressionInput& Input) {
				if (Pin == Index) Result.Source = &Input;
			});
			if (!Result.Source) return {.Message = "The expression input is unavailable."};
			if (Cast<DMaterialExpressionFunctionCall>(Expression)) return {.Message = "A function input requires a port identity."};
			Result.Numeric = FindMaterialNumericInput(*Expression, *Result.Source);
			Result.Default = Result.Numeric ? &Result.Numeric->Constant : nullptr;
			if (Result.Numeric)
			{
				const auto Catalog = EnumerateCatalog();
				const auto Shape = std::ranges::find(Catalog, Expression->GetClass(), &FMaterialGraphCatalogEntry::ExpressionClass);
				if (Shape != Catalog.end())
				{
					const auto* TypeProperty = Expression->GetClass()->FindPropertyByName("ResultType");
					auto Type = TypeProperty ? *static_cast<const EMaterialProgramValueType*>(TypeProperty->GetValuePtr(Expression)) : Shape->ResultType;
					if (const auto* Swizzle = Cast<DMaterialExpressionSwizzle>(Expression)) Type = static_cast<EMaterialProgramValueType>(Swizzle->Components.size() - 1);
					Result.Inherited = GetMaterialNumericInputFallback(Shape->Opcode, Type, Index);
				}
			}
			return Result;
		}

		auto CommitInputLink(DObject& Owner, DTransactor* Transactions, std::string Description,
			FMaterialExpressionCollection& Candidate, FMaterialRootInputs* Root,
			DMaterialExpression* Expression, FMaterialExpressionInput& Slot, FMaterialExpressionInput Next,
			std::vector<FMaterialExpressionFunctionInputBinding>& Bindings) -> FMaterialGraphCommandResult
		{
			if (Root)
			{
				Slot = Next;
				return CommitGraphEdit(Owner, Transactions, std::move(Description), Candidate, Root);
			}
			if (auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression))
			{
				Slot = Next;
				return CommitGraphEdit(Owner, Transactions, std::move(Description), Candidate, nullptr, {}, {},
					{Call}, [&] { Call->FunctionInputs.swap(Bindings); });
			}
			return CommitGraphEdit(Owner, Transactions, std::move(Description), Candidate, nullptr, {{&Slot, Next}});
		}

		auto MakeConstant(std::span<const float> Value, bool bTangentSpaceNormal) -> TStrongObjectPtr<DMaterialExpression>
		{
			TStrongObjectPtr<DMaterialExpression> Result;
			if (Value.size() == 1) { auto* E = NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None); E->Value = Value[0]; Result = E; }
			if (Value.size() == 2) { auto* E = NewObject<DMaterialExpressionVector2Constant>(nullptr, NAME_None); E->Value = FVector2(Value[0], Value[1]); Result = E; }
			if (Value.size() == 3)
			{
				auto* E = NewObject<DMaterialExpressionVector3Constant>(nullptr, NAME_None);
				E->Value = FVector3(Value[0], Value[1], Value[2]);
				E->bTangentSpaceNormal = bTangentSpaceNormal;
				Result = E;
			}
			if (Value.size() == 4) { auto* E = NewObject<DMaterialExpressionVector4Constant>(nullptr, NAME_None); E->Value = FVector4(Value[0], Value[1], Value[2], Value[3]); Result = E; }
			if (Result) Result->Id = FGuid::NewGuid();
			return Result;
		}

		auto ReadConstant(const DMaterialExpression* Source) -> std::vector<float>
		{
			if (const auto* E = Cast<DMaterialExpressionScalarConstant>(Source)) return {E->Value};
			if (const auto* E = Cast<DMaterialExpressionVector2Constant>(Source)) return {static_cast<float>(E->Value.x), static_cast<float>(E->Value.y)};
			if (const auto* E = Cast<DMaterialExpressionVector3Constant>(Source)) return {static_cast<float>(E->Value.x), static_cast<float>(E->Value.y), static_cast<float>(E->Value.z)};
			if (const auto* E = Cast<DMaterialExpressionVector4Constant>(Source)) return {static_cast<float>(E->Value.x), static_cast<float>(E->Value.y), static_cast<float>(E->Value.z), static_cast<float>(E->Value.w)};
			return {};
		}
	}

	auto GraphEditInternals::GetExpressionInputKey(const FMaterialGraphPinView& Input) -> FExpressionInputKey
	{
		if (Input.PortId.IsValid()) return Input.PortId;
		return Input.InputIndex;
	}

	namespace
	{
		auto SetConstant(DObject* Owner, DMaterialExpression* Expression, uint32 Index, std::optional<EMaterialProperty> Property,
			bool Enabled, ETransactionMode Mode) -> FMaterialGraphCommandResult
		{
			auto Admission = ResolveEditorTransactor(Mode);
			if (!Admission) return RejectCommand(Admission.error());
			auto* Transactions = *Admission;
			if (!Owner) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
			if (!Property && !IsExpressionMember(*Owner, Expression)) return RejectCommand("The input owner is unavailable.");
			std::optional<FMaterialRootInputs> RootEdit;
			if (Property) RootEdit = Cast<DMaterial>(Owner)->CaptureRootInputs();
			const auto Input = RootEdit ? FindRootInput(*RootEdit, *Property) : FindInput(Expression, Index);
			if (!Input.Message.empty()) return RejectCommand(Input.Message);
			if (!Input.Numeric || !Input.SupportsDefault()) return RejectCommand("This input does not support an editable constant.");
			if (Input.Numeric->UseConstant == Enabled) return {.Status = EMaterialGraphCommandStatus::NoChange};
			if (RootEdit)
			{
				Input.Numeric->UseConstant = Enabled;
				FMaterialExpressionCollection Candidate{FMaterialExpressionEditing::GetExpressions(*Owner)};
				return CommitGraphEdit(*Owner, Transactions, "Set Material Input Constant", Candidate, &*RootEdit);
			}
			auto Next = *Input.Numeric;
			Next.UseConstant = Enabled;
			return EditExpressionInputs(*Owner, Transactions, Enabled ? "Enable Input Constant" : "Use Definition Default", {Expression},
				[&] { *Input.Numeric = std::move(Next); }, {}, {{Input.Numeric, Next}});
		}

		auto WriteDefault(DObject* Owner, DMaterialExpression* Expression, FExpressionInputKey Key, std::optional<EMaterialProperty> Property,
			FMaterialInputDefault Value, ETransactionMode Mode) -> FMaterialGraphCommandResult
		{
			auto Admission = ResolveEditorTransactor(Mode);
			if (!Admission) return RejectCommand(Admission.error());
			auto* Transactions = *Admission;
			if (!Owner) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
			if (!Property && !IsExpressionMember(*Owner, Expression)) return RejectCommand("The input owner is unavailable.");
			std::optional<FMaterialRootInputs> Root;
			if (Property) Root = Cast<DMaterial>(Owner)->CaptureRootInputs();
			auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression);
			auto Bindings = Call ? Call->FunctionInputs : std::vector<FMaterialExpressionFunctionInputBinding>{};
			const auto Input = Root ? FindRootInput(*Root, *Property) : FindInput(Expression, Key, Call ? &Bindings : nullptr);
			if (!Input.Message.empty()) return RejectCommand(Input.Message);
			if (!Input.SupportsDefault()) return RejectCommand("This input does not support an inline numeric value.");
			std::vector<float> Components;
			if (Value.Kind == EMaterialInputDefaultKind::Literal && Value.Type <= EMaterialProgramValueType::Float4)
			{
				const std::array Values{Value.Literal.X, Value.Literal.Y, Value.Literal.Z, Value.Literal.W};
				Components.assign(Values.begin(), Values.begin() + static_cast<uint32>(Value.Type) + 1);
			}
			else if (Value.Kind != EMaterialInputDefaultKind::None) return RejectCommand("The input default is not numeric.");
			if (Input.Numeric && Components.empty() && !Input.Numeric->UseConstant) return {.Status = EMaterialGraphCommandStatus::NoChange};
			if (Input.Read() == Components && (!Input.Numeric || Input.Numeric->UseConstant == !Components.empty())) return {.Status = EMaterialGraphCommandStatus::NoChange};
			if (Root || Call)
			{
				if (!Input.Write(Components)) return RejectCommand("The input default has an incompatible width.");
				FMaterialExpressionCollection Candidate{FMaterialExpressionEditing::GetExpressions(*Owner)};
				if (Root) return CommitGraphEdit(*Owner, Transactions, "Edit Input Default", Candidate, &*Root);
				return CommitGraphEdit(*Owner, Transactions, "Edit Input Default", Candidate, nullptr, {}, {},
					{Call}, [&] { Call->FunctionInputs.swap(Bindings); });
			}
			auto Next = *Input.Numeric;
			if (Components.empty()) Next.UseConstant = false;
			else Next.SetConstant(Components);
			return EditExpressionInputs(*Owner, Transactions, "Edit Input Default", {Expression},
				[&] { *Input.Numeric = std::move(Next); }, {}, {{Input.Numeric, Next}});
		}

	}

	auto GraphEditInternals::WriteInputDefault(DObject* Owner, DMaterialExpression* Expression, FExpressionInputKey Input,
		FMaterialInputDefault Value, ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		return WriteDefault(Owner, Expression, Input, {}, std::move(Value), Mode);
	}

	auto GraphEditInternals::WriteInputDefault(DMaterial* Material, EMaterialProperty Property,
		FMaterialInputDefault Value, ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		return WriteDefault(Material, nullptr, uint32{0}, Property, std::move(Value), Mode);
	}

	auto GraphEditInternals::SetInputConstantEnabled(DObject* Owner, DMaterialExpression* Expression, uint32 Index,
		bool Enabled, ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		return SetConstant(Owner, Expression, Index, {}, Enabled, Mode);
	}

	auto GraphEditInternals::SetInputConstantEnabled(DMaterial* Material, EMaterialProperty Property,
		bool Enabled, ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		return SetConstant(Material, nullptr, 0, Property, Enabled, Mode);
	}

	auto DMaterialGraph::PromoteInputToParameter(const FMaterialGraphPin* Pin, ETransactionMode Mode)
		-> TMaterialGraphResult<std::vector<DMaterialGraphNode*>>
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return std::unexpected(RejectCommand(Admission.error()));
		auto* Material = Cast<DMaterial>(GetMaterialOrFunction());
		if (!Material) return std::unexpected(RejectCommand("Input parameters require a base material."));
		auto* Node = Pin ? Pin->GetOwningNode() : nullptr;
		if (!Node || FindNode(Node->GetView().Node.Id) != Node || Pin->Direction != EMaterialGraphPinDirection::Input
			|| Node->GetInputPin(Pin->DisplayIndex) != Pin || !Pin->LinkedTo.empty())
			return std::unexpected(RejectCommand("Select an unconnected input."));
		const auto& View = Node->GetView();
		const auto& Input = View.Inputs[Pin->DisplayIndex];
		if (Input.MaterialProperty && !MaterialEditing::CanPromoteSurfaceOutput(*Material, *Input.MaterialProperty))
			return std::unexpected(RejectCommand("This root input cannot be promoted to a numeric parameter."));
		if (Input.SourceType > EMaterialProgramValueType::Float4)
			return std::unexpected(RejectCommand("Only numeric inputs can be promoted."));
		auto Literal = Input.InlineDefault.Literal;
		const auto Type = Input.InlineDefault.Kind == EMaterialInputDefaultKind::Literal ? Input.InlineDefault.Type : Input.SourceType;
		if (Input.InlineDefault.Kind == EMaterialInputDefaultKind::None && Input.PortId.IsValid() && !Input.bRequired)
		{
			if (Input.Default.Kind == EMaterialFunctionDefaultKind::Numeric) Literal = Input.Default.Numeric;
			else if (Input.Default.Kind != EMaterialFunctionDefaultKind::None)
				return std::unexpected(RejectCommand("Override the inherited function default with a constant first."));
		}
		if (View.Node.Opcode == EMaterialProgramOpcode::MakeMaterialAttributes)
			Literal = FMaterialAttributeDefinitionMap::GetDefinitions()[Input.InputIndex].DefaultValue;
		if (View.Node.Opcode == EMaterialProgramOpcode::SetMaterialAttributes && Input.InputIndex != 0)
			return std::unexpected(RejectCommand("An unconnected attribute inherits its value from the incoming surface."));
		if (Input.bSupportsConstant && !Input.bUseConstant && View.Node.IsSampleUVInput(Input.InputIndex))
			return std::unexpected(RejectCommand("Enable a constant before promoting an inherited input."));

		FMaterialParameterDefinition Definition;
		Definition.Id = FGuid::NewGuid();
		Definition.Name = "Parameter";
		for (uint32 Suffix = 1; Material->FindParameterDefinition(Definition.Name); ++Suffix)
			Definition.Name = FName(std::format("Parameter{}", Suffix));
		Definition.DisplayName = Definition.Name.ToString();
		Definition.Value = MakeParameterValue(Type, Literal);
		Definition.Type = Definition.Value.GetType();
		auto Created = MakeParameterExpression(Definition);
		if (!Created) return std::unexpected(RejectCommand(std::move(Created.Message)));
		auto& Parameter = *Created.Expression;
		auto Candidate = Material->GetExpressionCollection();
		std::optional<FMaterialRootInputs> Root;
		if (Input.MaterialProperty) Root = Material->CaptureRootInputs();
		auto* Expression = Node->GetMaterialExpression();
		auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression);
		auto Bindings = Call ? Call->FunctionInputs : std::vector<FMaterialExpressionFunctionInputBinding>{};
		auto Destination = Root ? FindRootInput(*Root, *Input.MaterialProperty) : FindInput(Expression, GetExpressionInputKey(Input), Call ? &Bindings : nullptr);
		if (!Destination.Source) return std::unexpected(RejectCommand(std::move(Destination.Message)));
		if (Destination.Source->IsConnected()) return std::unexpected(RejectCommand("The input is already connected."));
		const bool NeedsMask = Type == EMaterialProgramValueType::Float2 || Type == EMaterialProgramValueType::Float3;
		const auto X = View.Presentation.X - 320;
		const auto Y = View.Presentation.Y;
		std::vector<FGuid> Generated{Parameter.Id};
		Candidate.Expressions.emplace_back(&Parameter);
		SetNodeLayout(Parameter, NeedsMask ? X - 320 : X, Y);
		FMaterialExpressionInput Next{&Parameter};
		TStrongObjectPtr<DMaterialExpression> Mask;
		if (NeedsMask)
		{
			Mask = MakeParameterMask(Parameter, Type);
			Next = {Mask.Get()};
			Candidate.Expressions.emplace_back(Mask.Get());
			SetNodeLayout(*Mask, X, Y);
			Generated.push_back(Mask->Id);
		}
		auto Result = CommitInputLink(*Material, *Admission, "Promote Input Parameter", Candidate, Root ? &*Root : nullptr,
			Expression, *Destination.Source, Next, Bindings);
		if (!Result) return std::unexpected(std::move(Result));
		return GetCreatedNodes(Generated);
	}

	auto DMaterialGraphNode::ExtractInputDefault(const FMaterialGraphPin* Pin, ETransactionMode Mode) -> TMaterialGraphResult<DMaterialGraphNode*>
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return std::unexpected(RejectCommand(Admission.error()));
		auto* Transactions = *Admission;
		auto* Graph = ResolveInputGraph(Pin);
		auto* Expression = GetMaterialExpression();
		if (!Graph) return std::unexpected(RejectCommand("The input pin is unavailable."));
		const auto& InputView = View.Inputs[Pin->DisplayIndex];
		auto& Owner = *Graph->GetMaterialOrFunction();
		FMaterialExpressionCollection Candidate{FMaterialExpressionEditing::GetExpressions(Owner)};
		std::optional<FMaterialRootInputs> Root;
		if (const auto* Material = Cast<DMaterial>(&Owner); Material && InputView.MaterialProperty) Root = Material->CaptureRootInputs();
		auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression);
		auto Bindings = Call ? Call->FunctionInputs : std::vector<FMaterialExpressionFunctionInputBinding>{};
		const auto Input = Root ? FindRootInput(*Root, *InputView.MaterialProperty) : FindInput(Expression, GetExpressionInputKey(InputView), Call ? &Bindings : nullptr);
		if (!Input.Message.empty()) return std::unexpected(RejectCommand(Input.Message));
		const bool bRootNormal = Input.RootProperty == EMaterialProperty::MP_Normal;
		const auto Value = Input.Read();
		if (Input.Source->IsConnected() || Value.empty())
			return std::unexpected(RejectCommand("Only an unconnected numeric literal can be extracted."));
		auto Constant = MakeConstant(Value, bRootNormal);
		if (!Constant) return std::unexpected(RejectCommand("The input default has an unsupported width."));
		SetNodeLayout(*Constant, View.Presentation.X - 320, View.Presentation.Y);
		const auto Id = Constant->Id;
		Candidate.Expressions.emplace_back(Constant.Get());
		auto Result = CommitInputLink(Owner, Transactions, "Extract Input Node", Candidate, Root ? &*Root : nullptr,
			Expression, *Input.Source, {Constant.Get()}, Bindings);
		if (!Result) return std::unexpected(std::move(Result));
		return Graph->GetCreatedNode(Id);
	}

	auto DMaterialGraphNode::InlineInputNode(const FMaterialGraphPin* Pin, ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		auto* Graph = ResolveInputGraph(Pin);
		auto* Expression = GetMaterialExpression();
		if (!Graph) return RejectCommand("The input pin is unavailable.");
		const auto& InputView = View.Inputs[Pin->DisplayIndex];
		auto& Owner = *Graph->GetMaterialOrFunction();
		FMaterialExpressionCollection Candidate{FMaterialExpressionEditing::GetExpressions(Owner)};
		std::optional<FMaterialRootInputs> Root;
		if (const auto* Material = Cast<DMaterial>(&Owner); Material && InputView.MaterialProperty) Root = Material->CaptureRootInputs();
		auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression);
		auto Bindings = Call ? Call->FunctionInputs : std::vector<FMaterialExpressionFunctionInputBinding>{};
		const auto Input = Root ? FindRootInput(*Root, *InputView.MaterialProperty) : FindInput(Expression, GetExpressionInputKey(InputView), Call ? &Bindings : nullptr);
		if (!Input.Message.empty()) return RejectCommand(Input.Message);
		if (!Input.SupportsDefault() || Input.Source->OutputIndex != 0)
			return RejectCommand("This source cannot be represented by an inline binding.");
		const auto* Source = Input.Source->Expression.Get();
		if (!Source || !std::ranges::contains(Candidate.Expressions, Source, [](const auto& Member) { return Member.Get(); }))
			return RejectCommand("The source expression is unavailable.");
		const auto Value = ReadConstant(Source);
		if (Value.empty()) return RejectCommand("Only numeric constants can be inlined; parameter exposure remains an explicit node.");
		const auto* LiveLink = Root ? nullptr : FindInput(Expression, GetExpressionInputKey(InputView)).Source;
		std::optional<FMaterialNumericInput> Numeric;
		if (Root || Call)
		{
			if (!Input.Write(Value)) return RejectCommand("The constant has an incompatible width.");
			*Input.Source = {};
		}
		else
		{
			Numeric = *Input.Numeric;
			Numeric->SetConstant(Value);
			Numeric->Connection = {};
		}
		bool bRetained = false;
		if (!Root)
			if (const auto* Material = Cast<DMaterial>(&Owner)) Root = Material->CaptureRootInputs();
		if (Root)
		{
			const auto UsesSource = [&](EMaterialProperty Property) {
				return Root->GetExpressionInputForProperty(Property)->Expression.Get() == Source;
			};
			bRetained = UsesSource(EMaterialProperty::MP_MaterialAttributes)
				|| std::ranges::any_of(FMaterialAttributeDefinitionMap::GetDefinitions(), [&](const auto& Field) { return UsesSource(Field.Property); });
		}
		for (const auto& Node : Candidate.Expressions)
			VisitStoredMaterialInputs(*Node, [&](const FMaterialExpressionInput& Link) {
				bRetained |= &Link != LiveLink && Link.Expression.Get() == Source;
			});
		if (!bRetained)
			std::erase_if(Candidate.Expressions, [&](const auto& Node) { return Node.Get() == Source; });
		FProposedLinks Links;
		if (LiveLink) Links.emplace(LiveLink, FMaterialExpressionInput{});
		FProposedNumerics Numerics;
		if (Numeric) Numerics.emplace(Input.Numeric, *Numeric);
		const auto Apply = [&] {
			if (Call) Call->FunctionInputs.swap(Bindings);
			else if (Numeric) *Input.Numeric = std::move(*Numeric);
		};
		return CommitGraphEdit(Owner, Transactions, "Inline Input Node", Candidate, Root ? &*Root : nullptr,
			std::move(Links), {}, Call ? std::vector<DMaterialExpression*>{Call} : std::vector<DMaterialExpression*>{}, Apply, Numerics);
	}

}
