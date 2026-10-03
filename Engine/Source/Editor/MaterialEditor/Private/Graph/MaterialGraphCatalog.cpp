#include "MaterialGraphDocument.h"

#include "Graph/MaterialGraphValueTypes.h"
#include "Asset/AssetPicker.h"
#include "Misc/StringHelper.h"
#include "Materials/MaterialExpressionInputs.h"
#include "MaterialGraphExpressionRegistry.h"

namespace Durin::Editor::Material
{
	auto IsGraphInputCompatible(std::span<const EMaterialProgramValueType> Accepted,
		EMaterialProgramValueType Source) -> bool
	{
		return FMaterialGraphSchema::Accepts(Accepted, Source);
	}

	auto MakeCreationAction(const FMaterialGraphCatalogEntry& Entry) -> FMaterialGraphCreationAction
	{
		return {.Id = std::format("expression:{}:{}", Entry.ExpressionClass ? Entry.ExpressionClass->GetName() : std::string{},
			IsMaterialAdaptiveNumeric(Entry.Opcode) ? 0u : static_cast<uint32>(Entry.ResultType)),
			.Name = Entry.OperationName, .Category = Entry.Category,
			.Keywords = std::string(GetProgramTypeName(Entry.ResultType)) + " " + Entry.Description,
			.Description = Entry.Description,
			.bFunction = Entry.Opcode != EMaterialProgramOpcode::Parameter
				&& Entry.Opcode != EMaterialProgramOpcode::TextureParameter
				&& Entry.Opcode != EMaterialProgramOpcode::TextureSampleParameter2D, .Payload = Entry};
	}
	auto MakeFunctionCreationAction(std::string Path) -> FMaterialGraphCreationAction
	{
		return {.Id = "function:" + Path,
			.Name = AssetPicker::GetAssetPathDisplayName(Path, EAssetPathDisplayMode::AssetName),
			.Category = "Functions",
			.Keywords = "function call " + Path, .Payload = std::move(Path)};
	}
	auto MakeCollectionParameterCreationAction(std::string Path,
		FGuid ParameterId, std::string ParameterName)
		-> FMaterialGraphCreationAction
	{
		const std::string CollectionName = AssetPicker::GetAssetPathDisplayName(
			Path, EAssetPathDisplayMode::AssetName);
		return {
			.Id = std::format("collection:{}:{}", Path, ParameterId.ToString()),
			.Name = ParameterName,
			.Category = "Parameters",
			.Keywords = "collection parameter " + CollectionName + " "
				+ ParameterName,
			.Description = "Reads " + ParameterName + " from " + CollectionName
				+ " in the current world.",
			.Payload = FMaterialGraphCollectionParameterCreation{
				std::move(Path), ParameterId, std::move(ParameterName)},
		};
	}
	auto MakePortCreationAction(bool bOutput, EMaterialProgramValueType Type) -> FMaterialGraphCreationAction
	{
		return {.Id = std::format("port:{}:{}", bOutput ? "output" : "input", static_cast<uint32>(Type)), .Name = bOutput ? "Function Output" : "Function Input",
			.Category = "Functions", .Keywords = "function port", .bMaterial = false,
			.Payload = FMaterialGraphPortCreation{bOutput, Type}};
	}
	namespace
	{
		auto MakeCatalogEntry(const FMaterialGraphExpressionRegistration& Registration,
			const FMaterialExpressionAuthoringShape& Shape) -> FMaterialGraphCatalogEntry
		{
			const auto& Description = *Registration.Description;
			FMaterialGraphCatalogEntry Entry;
			Entry.ExpressionClass = Description.ExpressionClass;
			Entry.OperationName = Description.Name;
			Entry.Category = Registration.Category;
			Entry.Description = Description.Description;
			Entry.Opcode = *Description.SemanticOpcode;
			Entry.ResultType = Shape.ResultType;
			Entry.InputNames = Shape.InputNames;
			Entry.AcceptedInputTypes = Shape.AcceptedInputTypes;
			if (Entry.Opcode == EMaterialProgramOpcode::Parameter)
			{
				Entry.OperationName = Shape.ResultType == EMaterialProgramValueType::Float ? "Scalar Parameter" : "Vector Parameter";
				Entry.Description = "Create a new numeric parameter exposed to material instances.";
			}
			return Entry;
		}

		auto NormalizeSearchText(std::string_view Value) -> std::string
		{
			return StringUtils::FoldAscii(Value);
		}

		auto GetDescriptionSearchField(const FMaterialGraphCatalogEntry& Entry) -> std::string
		{
			const auto* Descriptor = FindMaterialExpressionDescription(Entry.ExpressionClass);
			const auto Keywords = Descriptor ? Descriptor->SearchKeywords : std::string_view{};
			return NormalizeSearchText(Keywords.empty() ? Entry.Description : Entry.Description + " " + std::string(Keywords));
		}

		auto PrepareSearchFields(FMaterialGraphCatalogEntry& Entry) -> void
		{
			Entry.NormalizedSearchFields = {
				NormalizeSearchText(Entry.OperationName),
				NormalizeSearchText(Entry.Category),
				GetDescriptionSearchField(Entry),
				NormalizeSearchText(GetProgramTypeName(Entry.ResultType)),
			};
		}

	}

	auto FMaterialGraphNodeDescriptor::GetConstantLiteral() const -> FMaterialProgramLiteral
	{
		const auto* Value = std::get_if<FMaterialParameterValue>(&Data);
		return Value ? ReadParameterLiteral(ResultType, *Value) : FMaterialProgramLiteral{};
	}

	auto FMaterialGraphDocument::Inspect() const -> FMaterialGraphView
	{
		return Inspect(FMaterialGraphOperations::EnumerateCatalog());
	}

	auto FMaterialGraphDocument::Inspect(std::span<const FMaterialGraphCatalogEntry> Catalog) const -> FMaterialGraphView
	{
		return InspectSelection(Catalog, nullptr);
	}

	auto FMaterialGraphDocument::InspectNodes(std::span<const FGuid> Nodes,
		std::span<const FMaterialGraphCatalogEntry> Catalog) const -> FMaterialGraphView
	{
		const std::unordered_set<FGuid> Selection(Nodes.begin(), Nodes.end());
		return InspectSelection(Catalog, &Selection);
	}

	auto FMaterialGraphDocument::InspectSelection(std::span<const FMaterialGraphCatalogEntry> Catalog,
		const std::unordered_set<FGuid>* Selection) const -> FMaterialGraphView
	{
		const auto* Material = Cast<DMaterial>(Owner.Get());
		const auto* Function = Cast<DMaterialFunction>(Owner.Get());
		if (!Material && !Function) return {};
		const auto& Expressions = Material ? Material->GetExpressionCollection().Expressions : Function->GetExpressionCollection().Expressions;
		FMaterialGraphPresentation Presentation;
		if (Material) Presentation = Material->GetMaterialGraphPresentation();
		else Presentation.Nodes = Function->GetFunctionPresentation().Nodes;
		std::vector<FMaterialGraphNodeDescriptor> Descriptors;
		Descriptors.reserve(Expressions.size());
		std::unordered_map<FGuid, DMaterialExpression*> ExpressionsById;
		for (const auto& Expression : Expressions)
		{
			FMaterialGraphNodeDescriptor Node{.Id = Expression->Id, .bMaterialOutput = Cast<DMaterialExpressionMaterialOutput>(Expression.Get()) != nullptr};
			const auto* Description = FindMaterialExpressionDescription(Expression->GetClass());
			if (Description)
			{
				if (Description->SemanticOpcode) Node.Opcode = *Description->SemanticOpcode;
				if (!Description->Shapes.empty()) Node.ResultType = Description->Shapes.front().ResultType;
			}
			if (auto* Type = Expression->GetClass()->FindPropertyByName("ResultType"))
				Node.ResultType = *static_cast<const EMaterialProgramValueType*>(Type->GetValuePtr(Expression.Get()));
			if (const auto* Constant = Cast<DMaterialExpressionScalarConstant>(Expression.Get())) Node.Data = FMaterialParameterValue::MakeScalar(Constant->Value);
			else if (const auto* Constant = Cast<DMaterialExpressionVector2Constant>(Expression.Get())) Node.Data = FMaterialParameterValue::MakeVector2(Constant->Value);
			else if (const auto* Constant = Cast<DMaterialExpressionVector3Constant>(Expression.Get())) Node.Data = FMaterialParameterValue::MakeVector(Constant->Value);
			else if (const auto* Constant = Cast<DMaterialExpressionVector4Constant>(Expression.Get())) Node.Data = FMaterialParameterValue::MakeVector4(Constant->Value);
			else if (const auto* Sample = Cast<DMaterialExpressionTextureSampleParameter2D>(Expression.Get())) Node.Data = FMaterialGraphSampleInfo{Sample->Metadata.Id};
			else if (const auto* Sample = Cast<DMaterialExpressionTextureSample2D>(Expression.Get())) Node.Data = FMaterialGraphSampleInfo{};
			else if (const auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get())) Node.Data = FMaterialGraphParameterInfo{Parameter->Metadata.Id};
			else if (const auto* Swizzle = Cast<DMaterialExpressionSwizzle>(Expression.Get()))
			{
				Node.Data = Swizzle->Components;
				if (!Swizzle->Components.empty()) Node.ResultType = static_cast<EMaterialProgramValueType>(Swizzle->Components.size() - 1);
			}
			if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression.Get()))
			{
				Node.Opcode = EMaterialProgramOpcode::FunctionCall;
				if (!Call->Outputs.empty()) Node.ResultType = Call->Outputs.front().ExpectedType;
			}
			if (const auto* Collection = Cast<DMaterialExpressionCollectionParameter>(Expression.Get()); Collection && IsValid(Collection->Collection.Get()))
				if (const auto* Declaration = Collection->Collection->FindDeclaration(Collection->ParameterId))
					Node.ResultType = GetProgramType(Declaration->Type);
			const auto* Input = Cast<DMaterialExpressionFunctionInput>(Expression.Get());
			const auto* Output = Cast<DMaterialExpressionFunctionOutput>(Expression.Get());
			if (Input || Output)
			{
				Node.Opcode = Input ? EMaterialProgramOpcode::FunctionInput : EMaterialProgramOpcode::FunctionOutput;
				Node.ResultType = Input ? Input->Port.Type : Output->Port.Type;
			}
			Descriptors.push_back(std::move(Node));
			ExpressionsById.emplace(Expression->Id, Expression.Get());
		}
		const auto LinkView = [](const FMaterialExpressionInput& Input) -> FMaterialProgramLink {
			return {Input.ExpressionId, Input.OutputIndex, Input.OutputId};
		};
		const auto DefaultView = [](std::span<const float> Values) -> FMaterialInputDefault {
			if (Values.empty() || Values.size() > 4) return {};
			FMaterialInputDefault Result{.Kind = EMaterialInputDefaultKind::Literal,
				.Type = static_cast<EMaterialProgramValueType>(Values.size() - 1)};
			const std::array Lanes{&Result.Literal.X, &Result.Literal.Y, &Result.Literal.Z, &Result.Literal.W};
			for (size_t Index = 0; Index < Values.size(); ++Index) *Lanes[Index] = Values[Index];
			return Result;
		};

		FMaterialGraphView Result;
		std::unordered_map<FGuid, const FMaterialGraphNodeDescriptor*> NodesById;
		for (const auto& Node : Descriptors) NodesById.emplace(Node.Id, &Node);
		const auto FindCall = [&](const FGuid& NodeId) -> const DMaterialExpressionFunctionCall* {
			const auto It = ExpressionsById.find(NodeId);
			return It == ExpressionsById.end() ? nullptr : Cast<DMaterialExpressionFunctionCall>(It->second);
		};
		const auto SourceType = [&](const FMaterialProgramLink& Link) {
			if (const auto* Call = FindCall(Link.SourceNodeId))
			{
				const auto Output = std::ranges::find(Call->Outputs, Link.SourceOutputId, &FMaterialFunctionOutputBinding::OutputId);
				if (Output != Call->Outputs.end()) return Output->ExpectedType;
			}
			const auto Source = NodesById.find(Link.SourceNodeId);
			if (Source == NodesById.end()) return EMaterialProgramValueType::Float;
			if (const auto* Output = FindMaterialSampleOutput(Source->second->Opcode, Link.SourceOutputIndex))
				return Output->Type;
			if (Source->second->Opcode == EMaterialProgramOpcode::GetSurfaceAttributes && Link.SourceOutputIndex < 8)
				return GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(Link.SourceOutputIndex));
			return Source->second->ResultType;
		};
		constexpr std::array AttributeNames{"Base Color", "Normal", "Metallic", "Roughness", "Ambient Occlusion", "Emissive", "Opacity", "Opacity Mask"};
		std::unordered_map<const DClass*, std::vector<const FMaterialGraphCatalogEntry*>> BaseShapes;
		for (const auto& Entry : Catalog) BaseShapes[Entry.ExpressionClass].push_back(&Entry);
		std::unordered_map<FGuid, FMaterialGraphNodePresentation> Positions;
		Positions.reserve(Presentation.Nodes.size());
		for (const FMaterialGraphNodePresentation& Position : Presentation.Nodes)
			Positions.emplace(Position.NodeId, Position);
		Result.Nodes.reserve(Descriptors.size());
		for (size_t Ordinal = 0; Ordinal < Descriptors.size(); ++Ordinal)
		{
			const auto& Node = Descriptors[Ordinal];
			if (Selection && !Selection->contains(Node.Id)) continue;
			FMaterialGraphNodeView View{.Node = Node};
			if (Node.bMaterialOutput)
			{
				View.PrimaryLabel = "Material Output";
				auto* Terminal = Cast<DMaterialExpressionMaterialOutput>(ExpressionsById.at(Node.Id));
				for (const auto& Definition : GetMaterialDomainOutputPins(Material->GetDomain()))
				{
					const bool bAttributes = Definition.Id == EMaterialOutputPin::Surface;
					if (Definition.Id != EMaterialOutputPin::WorldPositionOffset
						&& bAttributes != Terminal->Outputs.bUseMaterialAttributes) continue;
					const auto& Input = *GetMaterialOutputInput(Terminal->Outputs, Definition.Id);
					FMaterialGraphPinView Pin{.InputIndex = static_cast<uint32>(Definition.Id), .Name = std::string(Definition.Name),
						.Link = LinkView(Input), .SourceType = SourceType(LinkView(Input)), .AcceptedTypes = {Definition.Type}};
					if (Definition.Id == EMaterialOutputPin::WorldPositionOffset)
						Pin.Constraint.Stages = EMaterialEvaluationStage::Vertex;
					else if (!bAttributes)
					{
						const auto Semantics = GetMaterialSurfaceOutputSemantics(
							static_cast<EMaterialSurfaceOutput>(Definition.Id));
						Pin.Constraint = {.Mode = EMaterialFunctionValueConstraintMode::Exact,
							.Stages = Semantics.Stages, .Kind = Semantics.Kind,
							.Space = Semantics.Space};
					}
					if (Definition.Id != EMaterialOutputPin::WorldPositionOffset
						&& Definition.Type > EMaterialProgramValueType::Float && Definition.Type <= EMaterialProgramValueType::Float4)
						Pin.AcceptedTypes.push_back(EMaterialProgramValueType::Float);
					Pin.InlineDefault = DefaultView(ReadMaterialOutputDefault(Terminal->Outputs, Definition.Id));
					if (const auto* Numeric = GetMaterialOutputNumericInput(const_cast<FMaterialExpressionSurfaceOutputs&>(Terminal->Outputs), Definition.Id))
					{
						Pin.bSupportsConstant = true; Pin.bUseConstant = Numeric->UseConstant;
						Pin.RetainedConstant = DefaultView(Numeric->Constant);
					}
					Pin.bActive = bAttributes || Definition.Id == EMaterialOutputPin::WorldPositionOffset || IsMaterialSurfaceOutputActive(
						static_cast<EMaterialSurfaceOutput>(Definition.Id), Material->GetStaticProperties());
					View.Inputs.push_back(std::move(Pin));
				}
				const auto Position = Positions.find(Node.Id);
				View.Presentation = Position != Positions.end() ? Position->second : FMaterialGraphNodePresentation{Node.Id, 1280, 0};
				Result.Nodes.push_back(std::move(View));
				continue;
			}
			auto* Expression = ExpressionsById.at(Node.Id);
			const auto* Descriptor = FindMaterialExpressionDescription(Expression->GetClass());
			const FMaterialGraphCatalogEntry* Shape = nullptr;
			const auto ShapeIt = BaseShapes.find(Expression->GetClass());
			if (ShapeIt != BaseShapes.end())
				for (const auto* Candidate : ShapeIt->second)
					if (Candidate->ResultType == Node.ResultType) { Shape = Candidate; break; }
			std::optional<FMaterialGraphCatalogEntry> FallbackShape;
			if (!Shape)
				if (const auto* Registration = FindMaterialGraphExpressionRegistration(Expression->GetClass()))
					for (const auto& Candidate : Registration->Description->Shapes)
						if (Candidate.ResultType == Node.ResultType)
						{
							FallbackShape = MakeCatalogEntry(*Registration, Candidate);
							Shape = &*FallbackShape;
							break;
						}
			View.PrimaryLabel = Shape ? Shape->OperationName : Descriptor ? std::string(Descriptor->Name) : "Unknown";
			if (const auto* Swizzle = Cast<DMaterialExpressionSwizzle>(Expression))
			{
				View.PrimaryLabel += " ";
				for (const uint8 Component : Swizzle->Components)
					View.PrimaryLabel += Component < 4 ? std::string(1, "RGBA"[Component]) : "?";
			}
			if (const auto* Parameter = Cast<DMaterialExpressionParameter>(Expression))
			{
				View.SecondaryLabel = View.PrimaryLabel;
				View.PrimaryLabel = Parameter->Metadata.DisplayName.empty() ? Parameter->Metadata.Name.ToString() : Parameter->Metadata.DisplayName;
			}
			if (!Cast<DMaterialExpressionFunctionCall>(Expression))
				VisitMaterialExpressionInputs(*Expression, [&](uint32 InputIndex, FMaterialExpressionInput& Input) {
					// Surface override indices are stable attributes, not binding-array ordinals.
					if (Cast<DMaterialExpressionSetSurfaceAttributes>(Expression) && InputIndex != 0) return;
					FMaterialGraphPinView Pin{.InputIndex = InputIndex,
						.Name = Shape && InputIndex < Shape->InputNames.size() ? Shape->InputNames[InputIndex] : "Value",
						.Link = LinkView(Input), .SourceType = SourceType(LinkView(Input))};
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
					if (!Input.ExpressionId.IsValid() && Pin.InlineDefault.Kind != EMaterialInputDefaultKind::None) Pin.SourceType = Pin.InlineDefault.Type;
					else if (!Input.ExpressionId.IsValid() && !Pin.AcceptedTypes.empty()) Pin.SourceType = Pin.AcceptedTypes.front();
					View.Inputs.push_back(std::move(Pin));
				});
			if (Node.Opcode == EMaterialProgramOpcode::FunctionCall)
			{
				if (const auto* Call = FindCall(Node.Id))
				{
					if (IsValid(Call->Function.Get()))
					{
						View.FunctionPath = Call->Function->GetObjectPath();
						View.SecondaryLabel = View.PrimaryLabel;
						View.PrimaryLabel = Call->Function->GetName();
						auto Inputs = Call->Function->GetFunctionSignature().Inputs;
						auto Outputs = Call->Function->GetFunctionSignature().Outputs;
						const auto Order = [](const auto& A, const auto& B) { return A.DisplayOrder != B.DisplayOrder ? A.DisplayOrder < B.DisplayOrder : A.Id < B.Id; };
						std::ranges::stable_sort(Inputs, Order);
						std::ranges::stable_sort(Outputs, Order);
						for (const auto& Port : Inputs)
						{
							const auto Binding = std::ranges::find(Call->Inputs, Port.Id, &FMaterialExpressionFunctionInputBinding::InputId);
							const auto Link = Binding == Call->Inputs.end() ? FMaterialProgramLink{} : LinkView(Binding->Input);
							View.Inputs.push_back({.InputIndex = static_cast<uint32>(View.Inputs.size()), .Name = Port.Name,
								.Link = Link, .SourceType = Link.SourceNodeId.IsValid() ? SourceType(Link) : Port.Type,
								.AcceptedTypes = {Port.Type}, .PortId = Port.Id, .bRequired = Port.bRequired, .Default = Port.Default,
								.Constraint = Port.Constraint,
								.InlineDefault = Binding == Call->Inputs.end() ? FMaterialInputDefault{} : DefaultView(Binding->InputDefault),
								.bAdvanced = Port.bAdvanced});
						}
						for (const auto& Port : Outputs) View.Outputs.push_back({.PortId = Port.Id, .Name = Port.Name,
							.Type = Port.Type, .Constraint = Port.Constraint});
					}
					else View.SecondaryLabel = "Missing function";
					for (const auto& Binding : Call->Inputs)
						if (std::ranges::none_of(View.Inputs, [&](const auto& Pin) { return Pin.PortId == Binding.InputId; }))
							View.Inputs.push_back({.InputIndex = static_cast<uint32>(View.Inputs.size()), .Name = "Missing input",
								.Link = LinkView(Binding.Input), .SourceType = SourceType(LinkView(Binding.Input)), .AcceptedTypes = {Binding.ExpectedType},
								.PortId = Binding.InputId, .bMissing = true});
					for (const auto& Binding : Call->Outputs)
						if (std::ranges::none_of(View.Outputs, [&](const auto& Pin) { return Pin.PortId == Binding.OutputId; }))
							View.Outputs.push_back({.PortId = Binding.OutputId, .Name = "Missing output", .Type = Binding.ExpectedType, .bMissing = true});
				}
			}
			else if (IsMaterialSamplingNode(Node.Opcode))
			{
				for (const auto& Output : GetMaterialSampleOutputs(Node.Opcode))
					View.Outputs.push_back({.OutputIndex = static_cast<uint8>(Output.Id), .Name = Output.Name, .Type = Output.Type});
			}
			else if (Node.Opcode == EMaterialProgramOpcode::GetSurfaceAttributes)
			{
				for (uint8 Index = 0; Index < 8; ++Index)
					if (Cast<DMaterialExpressionGetSurfaceAttributes>(Expression)->AttributeMask & (1u << Index)) View.Outputs.push_back({.OutputIndex = Index,
						.Name = AttributeNames[Index], .Type = GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(Index))});
			}
			else if (Node.Opcode != EMaterialProgramOpcode::FunctionOutput)
				View.Outputs.push_back({.Name = GetProgramTypeName(Node.ResultType), .Type = Node.ResultType});
			if (Node.Opcode == EMaterialProgramOpcode::FunctionInput || Node.Opcode == EMaterialProgramOpcode::FunctionOutput)
			{
				const auto& Port = Node.Opcode == EMaterialProgramOpcode::FunctionInput
					? Cast<DMaterialExpressionFunctionInput>(Expression)->Port : Cast<DMaterialExpressionFunctionOutput>(Expression)->Port;
				View.SecondaryLabel = std::format("{} ({})", View.PrimaryLabel, GetProgramTypeName(Port.Type));
				if (!Port.Name.empty()) View.PrimaryLabel = Port.Name;
				if (!View.Outputs.empty()) View.Outputs[0].Name = Port.Name;
				if (!View.Inputs.empty()) { View.Inputs[0].Name = Port.Name; View.Inputs[0].AcceptedTypes = {Port.Type}; }
			}
			if ((Node.Opcode == EMaterialProgramOpcode::GetSurfaceAttributes
				|| Node.Opcode == EMaterialProgramOpcode::SetSurfaceAttributes) && !View.Inputs.empty())
			{
				View.Inputs[0].Name = "Surface";
				View.Inputs[0].AcceptedTypes = {EMaterialProgramValueType::Surface};
			}
			if (Node.Opcode == EMaterialProgramOpcode::SetSurfaceAttributes)
				for (const auto& Attribute : Cast<DMaterialExpressionSetSurfaceAttributes>(Expression)->Attributes)
				{
					const auto Index = static_cast<uint32>(Attribute.Attribute);
					if (Index < 8) View.Inputs.push_back({.InputIndex = Index + 1, .Name = AttributeNames[Index],
						.Link = LinkView(Attribute.Source), .SourceType = Attribute.Source.Connection.ExpressionId.IsValid()
							? SourceType(LinkView(Attribute.Source)) : GetMaterialSurfaceOutputType(Attribute.Attribute),
						.AcceptedTypes = {GetMaterialSurfaceOutputType(Attribute.Attribute)},
						.InlineDefault = Attribute.Source.UseConstant ? DefaultView(Attribute.Source.Constant) : FMaterialInputDefault{},
						.RetainedConstant = DefaultView(Attribute.Source.Constant), .bSupportsConstant = true,
						.bUseConstant = Attribute.Source.UseConstant});
				}
			for (auto& Pin : View.Inputs)
				if (Node.Opcode != EMaterialProgramOpcode::Normalize && Pin.AcceptedTypes.size() == 1
					&& Pin.AcceptedTypes.front() > EMaterialProgramValueType::Float
					&& Pin.AcceptedTypes.front() <= EMaterialProgramValueType::Float4)
					Pin.AcceptedTypes.push_back(EMaterialProgramValueType::Float);
			const auto It = Positions.find(Node.Id);
			View.Presentation = It != Positions.end() ? It->second : FMaterialGraphNodePresentation{.NodeId = Node.Id,
				.X = static_cast<int32>(Ordinal % 4) * 320, .Y = static_cast<int32>(Ordinal / 4) * 240};
			if (!Node.GetParameterId().IsValid() && Node.Opcode != EMaterialProgramOpcode::FunctionCall
				&& Node.Opcode != EMaterialProgramOpcode::FunctionInput && Node.Opcode != EMaterialProgramOpcode::FunctionOutput)
				View.SecondaryLabel = View.Presentation.DisplayName;
			Result.Nodes.push_back(std::move(View));
		}
		std::ranges::sort(Result.Nodes, {}, [](const FMaterialGraphNodeView& View) {
			return View.Node.Id;
		});
		if (Material)
		{
			const auto& Outputs = Material->GetExpressionOutputs();
			Result.Outputs.Surface = LinkView(Outputs.Surface);
			const std::array Links{Outputs.BaseColor, Outputs.Normal, Outputs.Metallic, Outputs.Roughness,
				Outputs.AmbientOcclusion, Outputs.Emissive, Outputs.Opacity, Outputs.OpacityMask};

			for (uint32 Index = 0; Index < 8; ++Index)
			{
				GetMaterialSurfaceOutputLink(Result.Outputs, static_cast<EMaterialSurfaceOutput>(Index)) = LinkView(Links[Index]);
				GetMaterialSurfaceOutputDefault(Result.Outputs, static_cast<EMaterialSurfaceOutput>(Index)) = [&] {
					const auto V = ReadMaterialOutputDefault(Outputs, static_cast<EMaterialOutputPin>(Index));
					return FMaterialProgramLiteral{V[0], V.size() > 1 ? V[1] : 0.f, V.size() > 2 ? V[2] : 0.f};
				}();
			}
		}
		Result.bFunction = Function != nullptr;
		return Result;
	}

	auto FMaterialGraphOperations::EnumerateCatalog()
		-> std::vector<FMaterialGraphCatalogEntry>
	{
		std::vector<FMaterialGraphCatalogEntry> Result;
		for (const auto& Registration : GetMaterialGraphExpressionRegistrations())
		{
			if (Registration.CreationKind != EMaterialGraphCreationKind::Direct) continue;
			for (const auto& Shape : Registration.Description->Shapes)
				Result.push_back(MakeCatalogEntry(Registration, Shape));
		}
		std::ranges::stable_sort(Result, {}, &FMaterialGraphCatalogEntry::OperationName);
		for (FMaterialGraphCatalogEntry& Entry : Result) PrepareSearchFields(Entry);
		return Result;
	}

	auto FMaterialGraphOperations::SearchCatalog(
		std::string_view Query,
		std::optional<EMaterialProgramValueType> SourceType)
		-> std::vector<FMaterialGraphCatalogEntry>
	{
		return SearchCatalog(EnumerateCatalog(), Query, SourceType);
	}

	auto FMaterialGraphOperations::SearchCatalog(
		std::span<const FMaterialGraphCatalogEntry> Catalog,
		std::string_view Query,
		std::optional<EMaterialProgramValueType> SourceType)
		-> std::vector<FMaterialGraphCatalogEntry>
	{
		const std::vector<size_t> Indices = SearchCatalogIndices(
			Catalog, Query, SourceType);
		std::vector<FMaterialGraphCatalogEntry> Result;
		Result.reserve(Indices.size());
		for (const size_t Index : Indices) Result.push_back(Catalog[Index]);
		return Result;
	}

	auto FMaterialGraphOperations::SearchCatalogIndices(
		std::span<const FMaterialGraphCatalogEntry> Catalog,
		std::string_view Query,
		std::optional<EMaterialProgramValueType> SourceType)
		-> std::vector<size_t>
	{
		const std::string Needle = NormalizeSearchText(Query);
		struct FRankedEntry
		{
			size_t Index = 0;
			uint8 Match = 3;
			size_t Ordinal = 0;
		};
		std::vector<FRankedEntry> Ranked;
		for (size_t Ordinal = 0; Ordinal < Catalog.size(); ++Ordinal)
		{
			const FMaterialGraphCatalogEntry& Entry = Catalog[Ordinal];
			const auto* Descriptor = FindMaterialGraphExpressionRegistration(Entry.ExpressionClass);
			if (!Descriptor || Descriptor->CreationKind != EMaterialGraphCreationKind::Direct) continue;
			switch (Descriptor->PaletteShape)
			{
			case EMaterialGraphPaletteShape::InspectOnly: continue;
			case EMaterialGraphPaletteShape::Scalar:
				if (Entry.ResultType != EMaterialProgramValueType::Float) continue;
				break;
			case EMaterialGraphPaletteShape::ScalarOrVector4:
				if (Entry.ResultType != EMaterialProgramValueType::Float
					&& Entry.ResultType != EMaterialProgramValueType::Float4) continue;
				break;
			case EMaterialGraphPaletteShape::Float2:
				if (Entry.ResultType != EMaterialProgramValueType::Float2) continue;
				break;
			case EMaterialGraphPaletteShape::Adaptive:
			case EMaterialGraphPaletteShape::AdaptiveVector:
				if (Entry.ResultType != SourceType.value_or(
					Descriptor->PaletteShape == EMaterialGraphPaletteShape::AdaptiveVector
						? EMaterialProgramValueType::Float2 : EMaterialProgramValueType::Float)) continue;
				break;
			case EMaterialGraphPaletteShape::All: break;
			}
			if (SourceType)
			{
				if (Entry.AcceptedInputTypes.empty()
					|| !IsGraphInputCompatible(Entry.AcceptedInputTypes.front(), *SourceType)) continue;
			}
			uint8 Match = Needle.empty() ? 3 : 4;
			std::array<std::string, 4> FallbackSearchFields;
			const std::array<std::string, 4>* SearchFields =
				&Entry.NormalizedSearchFields;
			if (std::ranges::all_of(*SearchFields,
				[](const std::string& Field) { return Field.empty(); }))
			{
				FallbackSearchFields = {
					NormalizeSearchText(Entry.OperationName),
					NormalizeSearchText(Entry.Category),
					GetDescriptionSearchField(Entry),
					NormalizeSearchText(GetProgramTypeName(Entry.ResultType)),
				};
				SearchFields = &FallbackSearchFields;
			}
			for (const std::string& Haystack : *SearchFields)
			{
				if (Haystack == Needle) Match = std::min<uint8>(Match, 0);
				else if (Haystack.starts_with(Needle)) Match = std::min<uint8>(Match, 1);
				else if (Haystack.find(Needle) != std::string::npos)
					Match = std::min<uint8>(Match, 2);
			}
			if (Match < 4) Ranked.push_back({Ordinal, Match, Ordinal});
		}
		std::ranges::sort(Ranked, [Catalog](const FRankedEntry& A,
			const FRankedEntry& B) {
			if (A.Match != B.Match) return A.Match < B.Match;
			const FMaterialGraphCatalogEntry& EntryA = Catalog[A.Index];
			const FMaterialGraphCatalogEntry& EntryB = Catalog[B.Index];
			if (EntryA.Category != EntryB.Category)
				return EntryA.Category < EntryB.Category;
			if (EntryA.OperationName != EntryB.OperationName)
				return EntryA.OperationName < EntryB.OperationName;
			if (EntryA.ResultType != EntryB.ResultType)
				return EntryA.ResultType < EntryB.ResultType;
			return A.Ordinal < B.Ordinal;
		});
		std::vector<size_t> Result;
		Result.reserve(Ranked.size());
		for (const FRankedEntry& Entry : Ranked) Result.push_back(Entry.Index);
		return Result;
	}
}
