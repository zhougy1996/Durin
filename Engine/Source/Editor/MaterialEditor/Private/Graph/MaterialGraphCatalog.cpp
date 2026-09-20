#include "MaterialGraphDocument.h"

#include "Graph/MaterialGraphValueTypes.h"
#include "Misc/StringHelper.h"
#include "MaterialExpressionInputs.h"

namespace Durin::Editor::Material
{
	auto IsGraphInputCompatible(std::span<const EMaterialProgramValueType> Accepted,
		EMaterialProgramValueType Source) -> bool
	{
		return FMaterialGraphSchema::Accepts(Accepted, Source);
	}

	auto MakeCreationAction(const FMaterialGraphCatalogEntry& Entry) -> FMaterialGraphCreationAction
	{
		return {.Id = std::format("expression:{}:{}", static_cast<uint32>(Entry.Opcode),
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
		return {.Id = "function:" + Path, .Name = Path, .Category = "Material Functions",
			.Keywords = "function call " + Path, .Payload = std::move(Path)};
	}
	auto MakePortCreationAction(bool bOutput, EMaterialProgramValueType Type) -> FMaterialGraphCreationAction
	{
		return {.Id = std::format("port:{}:{}", bOutput ? "output" : "input", static_cast<uint32>(Type)), .Name = bOutput ? "Function Output" : "Function Input",
			.Category = "Material Functions", .Keywords = "function port", .bMaterial = false,
			.Payload = FMaterialGraphPortCreation{bOutput, Type}};
	}
	namespace
	{
		auto GetInputNames(EMaterialProgramOpcode Opcode, size_t Count)
			-> std::vector<std::string>
		{
			std::vector<std::string> Names;
			switch (Opcode)
			{
			case EMaterialProgramOpcode::UVChannel: Names = {"Channel"}; break;
			case EMaterialProgramOpcode::MakeSurface: Names = {"Base Color", "Normal", "Metallic", "Roughness", "Ambient Occlusion", "Emissive", "Opacity", "Opacity Mask"}; break;
			case EMaterialProgramOpcode::GetSurfaceAttributes:
			case EMaterialProgramOpcode::SetSurfaceAttributes: Names = {"Surface"}; break;
			case EMaterialProgramOpcode::TextureSample2D: Names = {"Texture", "UV"}; break;
			case EMaterialProgramOpcode::TextureSampleParameter2D: Names = {"UV"}; break;
			case EMaterialProgramOpcode::TextureCoordinates: Names = {"Channel"}; break;
			case EMaterialProgramOpcode::AppendVector:
			case EMaterialProgramOpcode::Add:
			case EMaterialProgramOpcode::Subtract:
			case EMaterialProgramOpcode::Multiply:
			case EMaterialProgramOpcode::Divide:
			case EMaterialProgramOpcode::Minimum:
			case EMaterialProgramOpcode::Maximum: Names = {"A", "B"}; break;
			case EMaterialProgramOpcode::Clamp: Names = {"Value", "Min", "Max"}; break;
			case EMaterialProgramOpcode::Lerp: Names = {"A", "B", "Alpha"}; break;
			case EMaterialProgramOpcode::MakeFloat2: Names = {"X", "Y"}; break;
			case EMaterialProgramOpcode::MakeFloat3: Names = {"X", "Y", "Z"}; break;
			case EMaterialProgramOpcode::MakeFloat4: Names = {"X", "Y", "Z", "W"}; break;
			case EMaterialProgramOpcode::BlendNormalsRNM: Names = {"Base", "Detail"}; break;
			default: break;
			}
			if (Names.size() < Count) Names.resize(Count, "Value");
			if (Names.size() > Count) Names.resize(Count);
			return Names;
		}

		auto GetCategory(EMaterialProgramOpcode Opcode) -> const char*
		{
			switch (Opcode)
			{
			case EMaterialProgramOpcode::Constant:
			case EMaterialProgramOpcode::Parameter:
			case EMaterialProgramOpcode::TextureParameter:
			case EMaterialProgramOpcode::UVChannel:
			case EMaterialProgramOpcode::WorldPosition:
			case EMaterialProgramOpcode::Time:
			case EMaterialProgramOpcode::TextureCoordinates: return "Inputs";
			case EMaterialProgramOpcode::TextureSampleParameter2D:
			case EMaterialProgramOpcode::TextureSample2D:
			case EMaterialProgramOpcode::BlendNormalsRNM: return "Textures";
			case EMaterialProgramOpcode::MakeSurface:
			case EMaterialProgramOpcode::GetSurfaceAttributes:
			case EMaterialProgramOpcode::SetSurfaceAttributes: return "Surface";
			case EMaterialProgramOpcode::AppendVector:
			case EMaterialProgramOpcode::Swizzle:
			case EMaterialProgramOpcode::MakeFloat2:
			case EMaterialProgramOpcode::MakeFloat3:
			case EMaterialProgramOpcode::MakeFloat4:
			case EMaterialProgramOpcode::Splat2:
			case EMaterialProgramOpcode::Splat3:
			case EMaterialProgramOpcode::Splat4: return "Channels";
			default: return "Math";
			}
		}

		auto GetOpcodeName(EMaterialProgramOpcode Opcode) -> const char*
		{
			switch (Opcode)
			{
			case EMaterialProgramOpcode::Constant: return "Constant";
			case EMaterialProgramOpcode::Parameter: return "Parameter";
			case EMaterialProgramOpcode::TextureParameter: return "Texture Object Parameter";
			case EMaterialProgramOpcode::TextureSampleParameter2D: return "Texture Sample Parameter 2D";
			case EMaterialProgramOpcode::WorldPosition: return "World Position";
			case EMaterialProgramOpcode::Time: return "Time";
			case EMaterialProgramOpcode::TextureCoordinates: return "Texture Coordinates";
			case EMaterialProgramOpcode::TextureSample2D: return "Texture Sample 2D";
			case EMaterialProgramOpcode::Add: return "Add";
			case EMaterialProgramOpcode::Subtract: return "Subtract";
			case EMaterialProgramOpcode::Multiply: return "Multiply";
			case EMaterialProgramOpcode::Divide: return "Divide";
			case EMaterialProgramOpcode::Minimum: return "Minimum";
			case EMaterialProgramOpcode::Maximum: return "Maximum";
			case EMaterialProgramOpcode::Negate: return "Negate";
			case EMaterialProgramOpcode::OneMinus: return "One Minus";
			case EMaterialProgramOpcode::Absolute: return "Absolute";
			case EMaterialProgramOpcode::Saturate: return "Saturate";
			case EMaterialProgramOpcode::Normalize: return "Normalize";
			case EMaterialProgramOpcode::Clamp: return "Clamp";
			case EMaterialProgramOpcode::Lerp: return "Lerp";
			case EMaterialProgramOpcode::MakeFloat2: return "Make Vector";
			case EMaterialProgramOpcode::MakeFloat3: return "Make Vector";
			case EMaterialProgramOpcode::MakeFloat4: return "Make Vector";
			case EMaterialProgramOpcode::AppendVector: return "Append Vector";
			case EMaterialProgramOpcode::Swizzle: return "Component Mask";
			case EMaterialProgramOpcode::Splat2: return "Splat";
			case EMaterialProgramOpcode::Splat3: return "Splat";
			case EMaterialProgramOpcode::Splat4: return "Splat";
			case EMaterialProgramOpcode::BlendNormalsRNM: return "Blend Normals RNM";
			case EMaterialProgramOpcode::DecodeNormalRG: return "Decode Normal RG";
			case EMaterialProgramOpcode::UVChannel: return "UV Channel";
			case EMaterialProgramOpcode::Sine: return "Sine";
			case EMaterialProgramOpcode::Cosine: return "Cosine";
			case EMaterialProgramOpcode::MakeSurface: return "Make Surface";
			case EMaterialProgramOpcode::FunctionInput: return "Function Input";
			case EMaterialProgramOpcode::FunctionOutput: return "Function Output";
			case EMaterialProgramOpcode::FunctionCall: return "Function Call";
			case EMaterialProgramOpcode::GetSurfaceAttributes: return "Get Surface Attributes";
			case EMaterialProgramOpcode::SetSurfaceAttributes: return "Set Surface Attributes";
			}
			return "Unknown";
		}

		auto GetExpressionClass(EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type) -> DClass*
		{
			if (Opcode == EMaterialProgramOpcode::Constant || Opcode == EMaterialProgramOpcode::Parameter)
			{
				const auto Index = static_cast<size_t>(Type);
				if (Index >= 4) return nullptr;
				if (Opcode == EMaterialProgramOpcode::Constant)
					return (std::array{DMaterialExpressionScalarConstant::StaticClass(), DMaterialExpressionVector2Constant::StaticClass(),
						DMaterialExpressionVector3Constant::StaticClass(), DMaterialExpressionVector4Constant::StaticClass()})[Index];
				return Type == EMaterialProgramValueType::Float ? DMaterialExpressionScalarParameter::StaticClass() : DMaterialExpressionVector4Parameter::StaticClass();
			}
			switch (Opcode)
			{
			case EMaterialProgramOpcode::TextureParameter: return DMaterialExpressionTextureParameter::StaticClass();
			case EMaterialProgramOpcode::TextureSampleParameter2D: return DMaterialExpressionTextureSampleParameter2D::StaticClass();
			case EMaterialProgramOpcode::TextureSample2D: return DMaterialExpressionTextureSample2D::StaticClass();
			case EMaterialProgramOpcode::Add: return DMaterialExpressionAdd::StaticClass();
			case EMaterialProgramOpcode::Subtract: return DMaterialExpressionSubtract::StaticClass();
			case EMaterialProgramOpcode::Multiply: return DMaterialExpressionMultiply::StaticClass();
			case EMaterialProgramOpcode::Divide: return DMaterialExpressionDivide::StaticClass();
			case EMaterialProgramOpcode::Minimum: return DMaterialExpressionMinimum::StaticClass();
			case EMaterialProgramOpcode::Maximum: return DMaterialExpressionMaximum::StaticClass();
			case EMaterialProgramOpcode::Negate: return DMaterialExpressionNegate::StaticClass();
			case EMaterialProgramOpcode::OneMinus: return DMaterialExpressionOneMinus::StaticClass();
			case EMaterialProgramOpcode::Absolute: return DMaterialExpressionAbsolute::StaticClass();
			case EMaterialProgramOpcode::Saturate: return DMaterialExpressionSaturate::StaticClass();
			case EMaterialProgramOpcode::Normalize: return DMaterialExpressionNormalize::StaticClass();
			case EMaterialProgramOpcode::Clamp: return DMaterialExpressionClamp::StaticClass();
			case EMaterialProgramOpcode::Lerp: return DMaterialExpressionLerp::StaticClass();
			case EMaterialProgramOpcode::AppendVector: return DMaterialExpressionAppendVector::StaticClass();
			case EMaterialProgramOpcode::Swizzle: return DMaterialExpressionSwizzle::StaticClass();
			case EMaterialProgramOpcode::Splat2: return DMaterialExpressionSplat2::StaticClass();
			case EMaterialProgramOpcode::Splat3: return DMaterialExpressionSplat3::StaticClass();
			case EMaterialProgramOpcode::Splat4: return DMaterialExpressionSplat4::StaticClass();
			case EMaterialProgramOpcode::BlendNormalsRNM: return DMaterialExpressionBlendNormalsRNM::StaticClass();
			case EMaterialProgramOpcode::UVChannel: return DMaterialExpressionUVChannel::StaticClass();
			case EMaterialProgramOpcode::Sine: return DMaterialExpressionSine::StaticClass();
			case EMaterialProgramOpcode::Cosine: return DMaterialExpressionCosine::StaticClass();
			case EMaterialProgramOpcode::MakeSurface: return DMaterialExpressionMakeSurface::StaticClass();
			case EMaterialProgramOpcode::FunctionInput: return DMaterialExpressionFunctionInput::StaticClass();
			case EMaterialProgramOpcode::FunctionOutput: return DMaterialExpressionFunctionOutput::StaticClass();
			case EMaterialProgramOpcode::FunctionCall: return DMaterialExpressionFunctionCall::StaticClass();
			case EMaterialProgramOpcode::GetSurfaceAttributes: return DMaterialExpressionGetSurfaceAttributes::StaticClass();
			case EMaterialProgramOpcode::SetSurfaceAttributes: return DMaterialExpressionSetSurfaceAttributes::StaticClass();
			case EMaterialProgramOpcode::WorldPosition: return DMaterialExpressionWorldPosition::StaticClass();
			case EMaterialProgramOpcode::Time: return DMaterialExpressionTime::StaticClass();
			case EMaterialProgramOpcode::TextureCoordinates: return DMaterialExpressionTextureCoordinates::StaticClass();
			case EMaterialProgramOpcode::MakeFloat2: return DMaterialExpressionMakeVector2::StaticClass();
			case EMaterialProgramOpcode::MakeFloat3: return DMaterialExpressionMakeVector3::StaticClass();
			case EMaterialProgramOpcode::MakeFloat4: return DMaterialExpressionMakeVector4::StaticClass();
			default: return nullptr;
			}
		}

		auto MakeCatalogEntry(
			EMaterialProgramOpcode Opcode,
			EMaterialProgramValueType ResultType,
			const FMaterialProgramNodeSignature& Signature)
			-> FMaterialGraphCatalogEntry
		{
			FMaterialGraphCatalogEntry Entry;
			Entry.OperationName = GetOpcodeName(Opcode);
			Entry.Category = GetCategory(Opcode);
			switch (Opcode)
			{
			case EMaterialProgramOpcode::Constant: Entry.Description = "A literal numeric value. Choose Float, Float2, Float3, or Float4 from the node type menu."; break;
			case EMaterialProgramOpcode::Parameter: Entry.Description = "A value exposed by the material parameter definition."; break;
			case EMaterialProgramOpcode::TextureParameter: Entry.Description = "A texture resource for function inputs or multiple samples. For ordinary texture mapping, use Texture Sample Parameter 2D."; break;
			case EMaterialProgramOpcode::TextureSample2D: Entry.Description = "Samples a connected texture resource. For a standalone replaceable texture, use Texture Sample Parameter 2D."; break;
			case EMaterialProgramOpcode::TextureSampleParameter2D: Entry.Description = "Samples a named texture parameter with mesh UV0 or a connected Float2 UV expression. Outputs share one fetch."; break;
			case EMaterialProgramOpcode::TextureCoordinates: Entry.Description = "Reads a mesh UV channel as Float2. Apply transforms with upstream math nodes."; break;
			case EMaterialProgramOpcode::Add: Entry.Description = "Adds two values component by component."; break;
			case EMaterialProgramOpcode::Subtract: Entry.Description = "Subtracts the second value from the first."; break;
			case EMaterialProgramOpcode::Multiply: Entry.Description = "Multiplies two values component by component."; break;
			case EMaterialProgramOpcode::Divide: Entry.Description = "Divides the first value by the second."; break;
			case EMaterialProgramOpcode::Minimum: Entry.Description = "Returns the component-wise minimum."; break;
			case EMaterialProgramOpcode::Maximum: Entry.Description = "Returns the component-wise maximum."; break;
			case EMaterialProgramOpcode::Negate: Entry.Description = "Reverses the sign of a value."; break;
			case EMaterialProgramOpcode::OneMinus: Entry.Description = "Subtracts a value from one."; break;
			case EMaterialProgramOpcode::Absolute: Entry.Description = "Returns the absolute value."; break;
			case EMaterialProgramOpcode::Saturate: Entry.Description = "Clamps a value to the zero-to-one range."; break;
			case EMaterialProgramOpcode::Normalize: Entry.Description = "Returns a unit-length vector."; break;
			case EMaterialProgramOpcode::Clamp: Entry.Description = "Constrains a value between minimum and maximum inputs."; break;
			case EMaterialProgramOpcode::Lerp: Entry.Description = "Interpolates between two values."; break;
			case EMaterialProgramOpcode::MakeFloat2:
			case EMaterialProgramOpcode::MakeFloat3:
			case EMaterialProgramOpcode::MakeFloat4: Entry.Description = "Combines scalar inputs into a vector."; break;
			case EMaterialProgramOpcode::WorldPosition: Entry.Description = "Surface position in world space (Float3)."; break;
			case EMaterialProgramOpcode::Time: Entry.Description = "Elapsed real time in seconds (Float), updated every rendered view."; break;
			case EMaterialProgramOpcode::AppendVector: Entry.Description = "Concatenates A and B; output width follows the inputs (up to four components)."; break;
			case EMaterialProgramOpcode::Swizzle: Entry.Description = "Selects, repeats or reorders channels (Component Mask / Truncate)."; break;
			case EMaterialProgramOpcode::Splat2:
			case EMaterialProgramOpcode::Splat3:
			case EMaterialProgramOpcode::Splat4: Entry.Description = "Replicates a scalar across vector components."; break;
			case EMaterialProgramOpcode::BlendNormalsRNM: Entry.Description = "Blends two tangent-space normals with RNM."; break;
			case EMaterialProgramOpcode::DecodeNormalRG: Entry.Description = "Decodes a tangent-space normal from its RG channels."; break;
			case EMaterialProgramOpcode::UVChannel: Entry.Description = "Selects mesh UV channel 0-3 using an explicit scalar input, rounded and clamped."; break;
			case EMaterialProgramOpcode::Sine: Entry.Description = "Returns the component-wise sine in radians."; break;
			case EMaterialProgramOpcode::Cosine: Entry.Description = "Returns the component-wise cosine in radians."; break;
			case EMaterialProgramOpcode::MakeSurface: Entry.Description = "Combines eight explicit surface properties without hidden parameter access."; break;
			case EMaterialProgramOpcode::GetSurfaceAttributes: Entry.Description = "Reads selected attributes from a Surface."; break;
			case EMaterialProgramOpcode::SetSurfaceAttributes: Entry.Description = "Overrides selected attributes while retaining the base Surface."; break;
			case EMaterialProgramOpcode::FunctionInput: Entry.Description = "Reads an input from the function signature."; break;
			case EMaterialProgramOpcode::FunctionOutput: Entry.Description = "Publishes a value through the function signature."; break;
			case EMaterialProgramOpcode::FunctionCall: Entry.Description = "Evaluates a material function with its bound inputs."; break;
			}
			Entry.Opcode = Opcode;
			Entry.ResultType = ResultType;
			Entry.ExpressionClass = GetExpressionClass(Opcode, ResultType);
			Entry.InputNames = GetInputNames(Opcode, Signature.InputCount);
			if (Opcode == EMaterialProgramOpcode::TextureSampleParameter2D) Entry.InputNames = {"UV"};
			if (Opcode == EMaterialProgramOpcode::TextureCoordinates) Entry.InputNames = {"Channel"};
			for (uint8 Index = 0; Index < Signature.InputCount; ++Index)
			{
				Entry.AcceptedInputTypes.emplace_back(
					Signature.Inputs[Index].begin(), Signature.Inputs[Index].end());
				if (Opcode != EMaterialProgramOpcode::Normalize && Signature.Inputs[Index].size() == 1
					&& Signature.Inputs[Index].front() > EMaterialProgramValueType::Float
					&& Signature.Inputs[Index].front() <= EMaterialProgramValueType::Float4)
					Entry.AcceptedInputTypes.back().push_back(EMaterialProgramValueType::Float);
			}
			return Entry;
		}

		auto NormalizeSearchText(std::string_view Value) -> std::string
		{
			return StringUtils::FoldAscii(Value);
		}

		auto PrepareSearchFields(FMaterialGraphCatalogEntry& Entry) -> void
		{
			Entry.NormalizedSearchFields = {
				NormalizeSearchText(Entry.OperationName),
				NormalizeSearchText(Entry.Category),
				NormalizeSearchText(Entry.Description),
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
		std::optional<std::vector<FMaterialGraphCatalogEntry>> FallbackCatalog;
		const auto FindClassShape = [&](DClass* Class) -> const FMaterialGraphCatalogEntry* {
			const auto Found = std::ranges::find(Catalog, Class, &FMaterialGraphCatalogEntry::ExpressionClass);
			if (Found != Catalog.end()) return &*Found;
			if (Class == DMaterialExpressionFunctionInput::StaticClass() || Class == DMaterialExpressionFunctionOutput::StaticClass()
				|| Class == DMaterialExpressionFunctionCall::StaticClass()) return nullptr;
			if (!FallbackCatalog) FallbackCatalog = FMaterialGraphOperations::EnumerateCatalog();
			const auto Fallback = std::ranges::find(*FallbackCatalog, Class, &FMaterialGraphCatalogEntry::ExpressionClass);
			return Fallback == FallbackCatalog->end() ? nullptr : &*Fallback;
		};
		std::vector<FMaterialGraphNodeDescriptor> Descriptors;
		Descriptors.reserve(Expressions.size());
		std::unordered_map<FGuid, DMaterialExpression*> ExpressionsById;
		for (const auto& Expression : Expressions)
		{
			FMaterialGraphNodeDescriptor Node{.Id = Expression->Id, .bMaterialOutput = Cast<DMaterialExpressionMaterialOutput>(Expression.Get()) != nullptr};
			const auto* Shape = FindClassShape(Expression->GetClass());
			if (Shape) { Node.Opcode = Shape->Opcode; Node.ResultType = Shape->ResultType; }
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
		const auto BaseShapeKey = [](EMaterialProgramOpcode Opcode,
			EMaterialProgramValueType ResultType) {
			return static_cast<uint32>(Opcode) << 8
				| static_cast<uint32>(ResultType);
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
		std::unordered_map<uint32, const FMaterialGraphCatalogEntry*> BaseShapes;
		BaseShapes.reserve(Catalog.size());
		for (const auto& Entry : Catalog)
			BaseShapes.emplace(BaseShapeKey(Entry.Opcode, Entry.ResultType), &Entry);
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
					if (bAttributes != Terminal->Outputs.bUseMaterialAttributes) continue;
					const auto& Input = *GetMaterialOutputInput(Terminal->Outputs, Definition.Id);
					FMaterialGraphPinView Pin{.InputIndex = static_cast<uint32>(Definition.Id), .Name = std::string(Definition.Name),
						.Link = LinkView(Input), .SourceType = SourceType(LinkView(Input)), .AcceptedTypes = {Definition.Type}};
					if (Definition.Type > EMaterialProgramValueType::Float && Definition.Type <= EMaterialProgramValueType::Float4)
						Pin.AcceptedTypes.push_back(EMaterialProgramValueType::Float);
					Pin.InlineDefault = DefaultView(ReadMaterialOutputDefault(Terminal->Outputs, Definition.Id));
					Pin.bActive = bAttributes || IsMaterialSurfaceOutputActive(
						static_cast<EMaterialSurfaceOutput>(Definition.Id), Material->GetStaticProperties());
					View.Inputs.push_back(std::move(Pin));
				}
				const auto Position = Positions.find(Node.Id);
				View.Presentation = Position != Positions.end() ? Position->second : FMaterialGraphNodePresentation{Node.Id, 1280, 0};
				Result.Nodes.push_back(std::move(View));
				continue;
			}
			const FMaterialGraphCatalogEntry* Shape = nullptr;
			const auto ShapeIt = BaseShapes.find(BaseShapeKey(Node.Opcode, Node.ResultType));
			if (ShapeIt != BaseShapes.end()) Shape = ShapeIt->second;
			View.PrimaryLabel = Shape
				? Shape->OperationName : GetOpcodeName(Node.Opcode);
			auto* Expression = ExpressionsById.at(Node.Id);
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
					Expression->GetClass()->ForEachProperty([&](FProperty* Property) {
						if (Property->GetValuePtr(Expression) != &Input) return;
						auto* Default = Expression->GetClass()->FindPropertyByName(FName(Property->NamePrivate.ToString() + "Default"));
						if (Default && Default->GetKind() == DurinCodeGen::EPropertyGenFlags::Array
							&& static_cast<FArrayProperty*>(Default)->GetInner()->GetKind() == DurinCodeGen::EPropertyGenFlags::Float)
							Pin.InlineDefault = DefaultView(*static_cast<const std::vector<float>*>(Default->GetValuePtr(Expression)));
					});
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
								.InlineDefault = Binding == Call->Inputs.end() ? FMaterialInputDefault{} : DefaultView(Binding->InputDefault),
								.bAdvanced = Port.bAdvanced});
						}
						for (const auto& Port : Outputs) View.Outputs.push_back({.PortId = Port.Id, .Name = Port.Name, .Type = Port.Type});
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
						.Link = LinkView(Attribute.Source), .SourceType = SourceType(LinkView(Attribute.Source)),
						.AcceptedTypes = {GetMaterialSurfaceOutputType(Attribute.Attribute)}});
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
			const auto VectorLiteral = [](const FVector3& Value) -> FMaterialProgramLiteral { return {static_cast<float>(Value.x), static_cast<float>(Value.y), static_cast<float>(Value.z)}; };
			const std::array<FMaterialProgramLiteral, 8> Defaults{VectorLiteral(Outputs.BaseColorDefault), VectorLiteral(Outputs.NormalDefault),
				FMaterialProgramLiteral{Outputs.MetallicDefault}, FMaterialProgramLiteral{Outputs.RoughnessDefault}, FMaterialProgramLiteral{Outputs.AmbientOcclusionDefault},
				VectorLiteral(Outputs.EmissiveDefault), FMaterialProgramLiteral{Outputs.OpacityDefault}, FMaterialProgramLiteral{Outputs.OpacityMaskDefault}};
			for (uint32 Index = 0; Index < 8; ++Index)
			{
				GetMaterialSurfaceOutputLink(Result.Outputs, static_cast<EMaterialSurfaceOutput>(Index)) = LinkView(Links[Index]);
				GetMaterialSurfaceOutputDefault(Result.Outputs, static_cast<EMaterialSurfaceOutput>(Index)) = Defaults[Index];
			}
		}
		Result.bFunction = Function != nullptr;
		return Result;
	}

	auto FMaterialGraphOperations::EnumerateCatalog()
		-> std::vector<FMaterialGraphCatalogEntry>
	{
		std::vector<FMaterialGraphCatalogEntry> Result;
		for (uint8 OpcodeValue = static_cast<uint8>(EMaterialProgramOpcode::Constant);
			OpcodeValue <= static_cast<uint8>(EMaterialProgramOpcode::Time); ++OpcodeValue)
			for (uint8 TypeValue = static_cast<uint8>(EMaterialProgramValueType::Float);
				TypeValue <= static_cast<uint8>(EMaterialProgramValueType::Surface); ++TypeValue)
			{
				const auto Opcode = static_cast<EMaterialProgramOpcode>(OpcodeValue);
				const auto Type = static_cast<EMaterialProgramValueType>(TypeValue);
				const auto Signature = GetMaterialProgramNodeSignature(Opcode, Type);
				if (!Signature) continue;
				auto Entry = MakeCatalogEntry(Opcode, Type, *Signature);
				if (!Entry.ExpressionClass) continue; // Internal IR operations are not authored nodes.
				if (Opcode == EMaterialProgramOpcode::Parameter
					|| Opcode == EMaterialProgramOpcode::TextureParameter)
				{
					Entry.OperationName = Type == EMaterialProgramValueType::Float ? "Scalar Parameter"
						: Type == EMaterialProgramValueType::Float2 ? "Vector Parameter"
						: Type == EMaterialProgramValueType::Float3 ? "Vector Parameter"
						: Type == EMaterialProgramValueType::Float4 ? "Vector Parameter" : "Texture Object Parameter";
					if (Opcode == EMaterialProgramOpcode::Parameter)
						Entry.Description = "Create a new numeric parameter exposed to material instances.";
				}

				Result.push_back(std::move(Entry));
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
			// Keep dimensional shapes for inspection; the palette creates one scalar Constant.
			if (Entry.Opcode == EMaterialProgramOpcode::Constant
				&& Entry.ResultType != EMaterialProgramValueType::Float) continue;
			if (Entry.Opcode >= EMaterialProgramOpcode::MakeFloat2 && Entry.Opcode <= EMaterialProgramOpcode::MakeFloat4) continue;
			if (Entry.Opcode >= EMaterialProgramOpcode::Splat2 && Entry.Opcode <= EMaterialProgramOpcode::Splat4) continue;
			if (Entry.Opcode == EMaterialProgramOpcode::AppendVector && Entry.ResultType != EMaterialProgramValueType::Float2) continue;
			if (Entry.Opcode == EMaterialProgramOpcode::Parameter
				&& Entry.ResultType != EMaterialProgramValueType::Float
				&& Entry.ResultType != EMaterialProgramValueType::Float4) continue;
			if (Entry.Opcode == EMaterialProgramOpcode::Swizzle
				&& Entry.ResultType != EMaterialProgramValueType::Float) continue;
			if (IsMaterialAdaptiveNumeric(Entry.Opcode))
			{
				const auto Type = SourceType.value_or(Entry.Opcode == EMaterialProgramOpcode::Normalize
					? EMaterialProgramValueType::Float2 : EMaterialProgramValueType::Float);
				if (Entry.ResultType != Type) continue;
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
					NormalizeSearchText(Entry.Description),
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
