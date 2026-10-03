#include "MaterialGraphDocument.h"

#include "Graph/MaterialGraphValueTypes.h"
#include "Asset/AssetPicker.h"
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
		enum class EMaterialGraphOpcodePurpose { Authored, AssetBound, Internal };
		enum class EMaterialGraphPaletteShape { All, InspectOnly, Scalar, ScalarOrVector4, Float2, Adaptive, AdaptiveVector };

		struct FMaterialGraphOpcodeDescriptor
		{
			EMaterialProgramOpcode Opcode;
			const char* Name;
			const char* Category;
			const char* Description;
			DClass* (*ExpressionClass)();
			std::array<const char*, 8> InputNames;
			EMaterialGraphOpcodePurpose Purpose = EMaterialGraphOpcodePurpose::Authored;
			EMaterialGraphPaletteShape PaletteShape = EMaterialGraphPaletteShape::All;
		};

		// Editor metadata stays together; runtime signatures still own pin types and semantics.
		constexpr FMaterialGraphOpcodeDescriptor OpcodeDescriptors[] = {
			{EMaterialProgramOpcode::Constant, "Constant", "Inputs",
				"A literal numeric value. Choose Float, Float2, Float3, or Float4 from the node type menu.",
				nullptr, {}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Scalar},
			{EMaterialProgramOpcode::Parameter, "Parameter", "Parameters",
				"A value exposed by the material parameter definition.",
				nullptr, {}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::ScalarOrVector4},
			{EMaterialProgramOpcode::TextureParameter, "Texture Object Parameter", "Parameters",
				"A texture resource for function inputs or multiple samples. For ordinary texture mapping, use Texture Sample Parameter 2D.",
				&DMaterialExpressionTextureParameter::StaticClass, {}},
			{EMaterialProgramOpcode::TextureSampleParameter2D, "Texture Sample Parameter 2D", "Parameters",
				"Samples a named texture parameter with mesh UV0 or a connected Float2 UV expression. Outputs share one fetch.",
				&DMaterialExpressionTextureSampleParameter2D::StaticClass, {"UV"}},
			{EMaterialProgramOpcode::WorldPosition, "World Position", "Inputs",
				"Surface position relative to the current view origin (Float3).",
				&DMaterialExpressionWorldPosition::StaticClass, {}},
			{EMaterialProgramOpcode::Time, "Time", "Inputs",
				"Elapsed real time in seconds (Float), updated every rendered view.",
				&DMaterialExpressionTime::StaticClass, {}},
			{EMaterialProgramOpcode::CollectionParameter, "Collection Parameter", "Parameters",
				"Reads a numeric value from a material parameter collection in the current world.",
				nullptr, {}, EMaterialGraphOpcodePurpose::AssetBound, EMaterialGraphPaletteShape::InspectOnly},
			{EMaterialProgramOpcode::CameraPosition, "Camera Position", "Inputs",
				"Active pass camera position in translated world space; zero at the view origin (Float3).",
				&DMaterialExpressionCameraPosition::StaticClass, {}},
			{EMaterialProgramOpcode::CameraVector, "Camera Vector", "Inputs",
				"Direction from the fragment to the active pass camera (Float3).",
				&DMaterialExpressionCameraVector::StaticClass, {}},
			{EMaterialProgramOpcode::ObjectPosition, "Object Position", "Inputs",
				"Render primitive bounds center relative to the current view origin (Float3).",
				&DMaterialExpressionObjectPosition::StaticClass, {}},
			{EMaterialProgramOpcode::VertexInterpolator, "Vertex Interpolator", "Vertex",
				"Evaluates the input per vertex and interpolates its value to pixel calculations.",
				&DMaterialExpressionVertexInterpolator::StaticClass, {}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::VertexNormal, "Vertex Normal", "Inputs",
				"Post-vertex-factory world normal for vertex offsets or explicit interpolation.",
				&DMaterialExpressionVertexNormal::StaticClass, {}},
			{EMaterialProgramOpcode::ScreenPosition, "Screen Position", "Inputs",
				"Normalized position within the active pass viewport (Float2).",
				&DMaterialExpressionScreenPosition::StaticClass, {}},
			{EMaterialProgramOpcode::ViewSize, "View Size", "Inputs",
				"Active pass viewport size in pixels (Float2).",
				&DMaterialExpressionViewSize::StaticClass, {}},
			{EMaterialProgramOpcode::StaticBool, "Static Bool", "Static Selection",
				"Declares a root-owned compile-time boolean keyed by stable GUID.",
				&DMaterialExpressionStaticBool::StaticClass, {}},
			{EMaterialProgramOpcode::StaticSwitch, "Static Switch", "Static Selection",
				"Selects exactly one branch from a static bool before normalized MIR.",
				&DMaterialExpressionStaticSwitch::StaticClass, {"Condition", "False", "True"}},
			{EMaterialProgramOpcode::QualitySwitch, "Quality Switch", "Static Selection",
				"Selects Low or High, using Default when that branch is unconnected.",
				&DMaterialExpressionQualitySwitch::StaticClass, {"Default", "Low", "High"}},
			{EMaterialProgramOpcode::FeatureLevelSwitch, "Feature Level Switch", "Static Selection",
				"Selects the accepted RHI feature tier, using Default when unconnected.",
				&DMaterialExpressionFeatureLevelSwitch::StaticClass, {"Default", "ES3_1", "SM5", "SM6"}},
			{EMaterialProgramOpcode::TransformPosition, "Transform Position", "Transforms",
				"Transforms a spatial position between explicit coordinate spaces; World is relative to the current view origin.",
				&DMaterialExpressionTransformPosition::StaticClass, {"Input"}},
			{EMaterialProgramOpcode::TransformDirection, "Transform Direction", "Transforms",
				"Transforms a spatial direction between explicit coordinate spaces.",
				&DMaterialExpressionTransformDirection::StaticClass, {"Input"}},
			{EMaterialProgramOpcode::TransformNormal, "Transform Normal", "Transforms",
				"Transforms and normalizes a spatial normal with inverse-transpose semantics.",
				&DMaterialExpressionTransformNormal::StaticClass, {"Input"}},
			{EMaterialProgramOpcode::TextureCoordinates, "Texture Coordinates", "Inputs",
				"Reads a mesh UV channel as Float2. Apply transforms with upstream math nodes.",
				&DMaterialExpressionTextureCoordinates::StaticClass, {"Channel"}},
			{EMaterialProgramOpcode::TextureSample2D, "Texture Sample 2D", "Textures",
				"Samples a connected texture resource. For a standalone replaceable texture, use Texture Sample Parameter 2D.",
				&DMaterialExpressionTextureSample2D::StaticClass, {"Texture", "UV"}},
			{EMaterialProgramOpcode::Add, "Add", "Math",
				"Adds two values component by component.",
				&DMaterialExpressionAdd::StaticClass, {"A", "B"}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::Subtract, "Subtract", "Math",
				"Subtracts the second value from the first.",
				&DMaterialExpressionSubtract::StaticClass, {"A", "B"}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::Multiply, "Multiply", "Math",
				"Multiplies two values component by component.",
				&DMaterialExpressionMultiply::StaticClass, {"A", "B"}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::Divide, "Divide", "Math",
				"Divides the first value by the second.",
				&DMaterialExpressionDivide::StaticClass, {"A", "B"}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::Minimum, "Minimum", "Math",
				"Returns the component-wise minimum.",
				&DMaterialExpressionMinimum::StaticClass, {"A", "B"}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::Maximum, "Maximum", "Math",
				"Returns the component-wise maximum.",
				&DMaterialExpressionMaximum::StaticClass, {"A", "B"}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::Negate, "Negate", "Math",
				"Reverses the sign of a value.",
				&DMaterialExpressionNegate::StaticClass, {}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::OneMinus, "One Minus", "Math",
				"Subtracts a value from one.",
				&DMaterialExpressionOneMinus::StaticClass, {}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::Absolute, "Absolute", "Math",
				"Returns the absolute value.",
				&DMaterialExpressionAbsolute::StaticClass, {}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::Saturate, "Saturate", "Math",
				"Clamps a value to the zero-to-one range.",
				&DMaterialExpressionSaturate::StaticClass, {}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::Normalize, "Normalize", "Math",
				"Returns a unit-length vector.",
				&DMaterialExpressionNormalize::StaticClass, {}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::AdaptiveVector},
			{EMaterialProgramOpcode::Clamp, "Clamp", "Math",
				"Constrains a value between minimum and maximum inputs.",
				&DMaterialExpressionClamp::StaticClass, {"Value", "Min", "Max"}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::Lerp, "Lerp", "Math",
				"Interpolates between two values.",
				&DMaterialExpressionLerp::StaticClass, {"A", "B", "Alpha"}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::MakeFloat2, "Make Vector", "Channels",
				"Combines scalar inputs into a vector.",
				&DMaterialExpressionMakeVector2::StaticClass, {"X", "Y"}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::InspectOnly},
			{EMaterialProgramOpcode::MakeFloat3, "Make Vector", "Channels",
				"Combines scalar inputs into a vector.",
				&DMaterialExpressionMakeVector3::StaticClass, {"X", "Y", "Z"}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::InspectOnly},
			{EMaterialProgramOpcode::MakeFloat4, "Make Vector", "Channels",
				"Combines scalar inputs into a vector.",
				&DMaterialExpressionMakeVector4::StaticClass, {"X", "Y", "Z", "W"}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::InspectOnly},
			{EMaterialProgramOpcode::AppendVector, "Append Vector", "Channels",
				"Concatenates A and B; output width follows the inputs (up to four components).",
				&DMaterialExpressionAppendVector::StaticClass, {"A", "B"}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Float2},
			{EMaterialProgramOpcode::Swizzle, "Component Mask", "Channels",
				"Selects, repeats or reorders channels (Component Mask / Truncate).",
				&DMaterialExpressionSwizzle::StaticClass, {}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Scalar},
			{EMaterialProgramOpcode::Splat2, "Splat", "Channels",
				"Replicates a scalar across vector components.",
				&DMaterialExpressionSplat2::StaticClass, {}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::InspectOnly},
			{EMaterialProgramOpcode::Splat3, "Splat", "Channels",
				"Replicates a scalar across vector components.",
				&DMaterialExpressionSplat3::StaticClass, {}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::InspectOnly},
			{EMaterialProgramOpcode::Splat4, "Splat", "Channels",
				"Replicates a scalar across vector components.",
				&DMaterialExpressionSplat4::StaticClass, {}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::InspectOnly},
			{EMaterialProgramOpcode::BlendNormalsRNM, "Blend Normals RNM", "Textures",
				"Blends two tangent-space normals with RNM.",
				&DMaterialExpressionBlendNormalsRNM::StaticClass, {"Base", "Detail"}},
			{EMaterialProgramOpcode::DecodeNormalRG, "Decode Normal RG", "Math",
				"Decodes a tangent-space normal from its RG channels.",
				nullptr, {}, EMaterialGraphOpcodePurpose::Internal, EMaterialGraphPaletteShape::InspectOnly},
			{EMaterialProgramOpcode::UVChannel, "UV Channel", "Inputs",
				"Selects mesh UV channel 0-3 using an explicit scalar input, rounded and clamped.",
				&DMaterialExpressionUVChannel::StaticClass, {"Channel"}},
			{EMaterialProgramOpcode::Sine, "Sine", "Math",
				"Returns the component-wise sine in radians.",
				&DMaterialExpressionSine::StaticClass, {}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::Cosine, "Cosine", "Math",
				"Returns the component-wise cosine in radians.",
				&DMaterialExpressionCosine::StaticClass, {}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::Dot, "Dot Product", "Math",
				"Returns the scalar dot product of equal-width vectors.",
				&DMaterialExpressionDot::StaticClass, {"A", "B"}},
			{EMaterialProgramOpcode::Cross, "Cross Product", "Math",
				"Returns the cross product of two Float3 values.",
				&DMaterialExpressionCross::StaticClass, {"A", "B"}},
			{EMaterialProgramOpcode::Length, "Length", "Math",
				"Returns the scalar length of a vector.",
				&DMaterialExpressionLength::StaticClass, {}},
			{EMaterialProgramOpcode::Distance, "Distance", "Math",
				"Returns the scalar distance between equal-width values.",
				&DMaterialExpressionDistance::StaticClass, {"A", "B"}},
			{EMaterialProgramOpcode::Pow, "Power", "Math",
				"Raises each base component to its exponent.",
				&DMaterialExpressionPow::StaticClass, {"Base", "Exponent"}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::Sqrt, "Square Root", "Math",
				"Returns the component-wise square root.",
				&DMaterialExpressionSqrt::StaticClass, {}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::Exp, "Exponential", "Math",
				"Returns the component-wise natural exponential.",
				&DMaterialExpressionExp::StaticClass, {}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::Log, "Natural Log", "Math",
				"Returns the component-wise natural logarithm.",
				&DMaterialExpressionLog::StaticClass, {}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::Floor, "Floor", "Math",
				"Rounds each component down.",
				&DMaterialExpressionFloor::StaticClass, {}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::Ceil, "Ceil", "Math",
				"Rounds each component up.",
				&DMaterialExpressionCeil::StaticClass, {}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::Round, "Round", "Math",
				"Rounds each component to the nearest integer.",
				&DMaterialExpressionRound::StaticClass, {}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::Frac, "Fraction", "Math",
				"Returns each component's fractional part.",
				&DMaterialExpressionFrac::StaticClass, {}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::Fmod, "Fmod", "Math",
				"Returns the component-wise floating-point remainder.",
				&DMaterialExpressionFmod::StaticClass, {"A", "B"}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::Step, "Step", "Math",
				"Returns zero below Edge and one otherwise.",
				&DMaterialExpressionStep::StaticClass, {"Edge", "Value"}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::SmoothStep, "Smooth Step", "Math",
				"Returns smooth Hermite interpolation between Min and Max.",
				&DMaterialExpressionSmoothStep::StaticClass, {"Min", "Max", "Value"}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::Sign, "Sign", "Math",
				"Returns the sign of each component.",
				&DMaterialExpressionSign::StaticClass, {}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::Adaptive},
			{EMaterialProgramOpcode::Reflect, "Reflect", "Math",
				"Reflects an incident vector around a normal of equal width.",
				&DMaterialExpressionReflect::StaticClass, {"Incident", "Normal"}, EMaterialGraphOpcodePurpose::Authored, EMaterialGraphPaletteShape::AdaptiveVector},
			{EMaterialProgramOpcode::MakeSurface, "Make Surface", "Surface",
				"Combines eight explicit surface properties without hidden parameter access.",
				&DMaterialExpressionMakeSurface::StaticClass, {"Base Color", "Normal", "Metallic", "Roughness", "Ambient Occlusion", "Emissive", "Opacity", "Opacity Mask"}},
			{EMaterialProgramOpcode::FunctionInput, "Function Input", "Functions",
				"Reads an input from the function signature.",
				&DMaterialExpressionFunctionInput::StaticClass, {}},
			{EMaterialProgramOpcode::FunctionOutput, "Function Output", "Functions",
				"Publishes a value through the function signature.",
				&DMaterialExpressionFunctionOutput::StaticClass, {}},
			{EMaterialProgramOpcode::FunctionCall, "Function Call", "Functions",
				"Evaluates a material function with its bound inputs.",
				&DMaterialExpressionFunctionCall::StaticClass, {}},
			{EMaterialProgramOpcode::GetSurfaceAttributes, "Get Surface Attributes", "Surface",
				"Reads selected attributes from a Surface.",
				&DMaterialExpressionGetSurfaceAttributes::StaticClass, {"Surface"}},
			{EMaterialProgramOpcode::SetSurfaceAttributes, "Set Surface Attributes", "Surface",
				"Overrides selected attributes while retaining the base Surface.",
				&DMaterialExpressionSetSurfaceAttributes::StaticClass, {"Surface"}},
		};

		auto GetOpcodeDescriptor(EMaterialProgramOpcode Opcode)
			-> const FMaterialGraphOpcodeDescriptor&
		{
			const auto Found = std::ranges::find(OpcodeDescriptors, Opcode,
				&FMaterialGraphOpcodeDescriptor::Opcode);
			static constexpr FMaterialGraphOpcodeDescriptor Unknown{
				.Opcode = {}, .Name = "Unknown", .Category = "Math", .Description = "",
				.ExpressionClass = nullptr, .InputNames = {},
			};
			return Found != std::end(OpcodeDescriptors) ? *Found : Unknown;
		}

		auto GetTypedExpressionClass(EMaterialProgramOpcode Opcode,
			EMaterialProgramValueType Type) -> DClass*
		{
			if (Opcode == EMaterialProgramOpcode::Constant
				|| Opcode == EMaterialProgramOpcode::Parameter)
			{
				const auto Index = static_cast<size_t>(Type);
				if (Index >= 4) return nullptr;
				if (Opcode == EMaterialProgramOpcode::Constant)
					return (std::array{DMaterialExpressionScalarConstant::StaticClass(), DMaterialExpressionVector2Constant::StaticClass(),
						DMaterialExpressionVector3Constant::StaticClass(), DMaterialExpressionVector4Constant::StaticClass()})[Index];
				return Type == EMaterialProgramValueType::Float ? DMaterialExpressionScalarParameter::StaticClass() : DMaterialExpressionVector4Parameter::StaticClass();
			}
			return nullptr;
		}

		auto MakeCatalogEntry(
			EMaterialProgramOpcode Opcode,
			EMaterialProgramValueType ResultType,
			const FMaterialProgramNodeSignature& Signature)
			-> FMaterialGraphCatalogEntry
		{
			FMaterialGraphCatalogEntry Entry;
			const auto& Descriptor = GetOpcodeDescriptor(Opcode);
			Entry.OperationName = Descriptor.Name;
			Entry.Category = Descriptor.Category;
			Entry.Description = Descriptor.Description;
			Entry.Opcode = Opcode;
			Entry.ResultType = ResultType;
			Entry.ExpressionClass = Descriptor.ExpressionClass
				? Descriptor.ExpressionClass() : GetTypedExpressionClass(Opcode, ResultType);
			for (uint8 Index = 0; Index < Signature.InputCount; ++Index)
				Entry.InputNames.emplace_back(Descriptor.InputNames[Index]
					? Descriptor.InputNames[Index] : "Value");
			if (Opcode == EMaterialProgramOpcode::TextureSampleParameter2D) Entry.InputNames = {"UV"};
			if (Opcode == EMaterialProgramOpcode::TextureCoordinates) Entry.InputNames = {"Channel"};
			for (uint8 Index = 0; Index < Signature.InputCount; ++Index)
			{
				Entry.AcceptedInputTypes.emplace_back(
					Signature.Inputs[Index].begin(), Signature.Inputs[Index].end());
				const bool bExactNormal = Opcode == EMaterialProgramOpcode::BlendNormalsRNM
					|| (Opcode == EMaterialProgramOpcode::MakeSurface
						&& Index == static_cast<uint8>(EMaterialSurfaceOutput::Normal));
				if (MaterialNumericInputAllowsScalarBroadcast(Opcode, Index) && !bExactNormal
					&& Signature.Inputs[Index].size() == 1
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
			const FMaterialGraphCatalogEntry* Shape = nullptr;
			const auto ShapeIt = BaseShapes.find(BaseShapeKey(Node.Opcode, Node.ResultType));
			if (ShapeIt != BaseShapes.end()) Shape = ShapeIt->second;
			View.PrimaryLabel = Shape
				? Shape->OperationName : GetOpcodeDescriptor(Node.Opcode).Name;
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
		for (uint8 OpcodeValue = static_cast<uint8>(EMaterialProgramOpcode::Constant);
			OpcodeValue <= static_cast<uint8>(EMaterialProgramOpcode::VertexInterpolator); ++OpcodeValue)
			for (uint8 TypeValue = static_cast<uint8>(EMaterialProgramValueType::Float);
				TypeValue <= static_cast<uint8>(EMaterialProgramValueType::StaticBool); ++TypeValue)
			{
				const auto Opcode = static_cast<EMaterialProgramOpcode>(OpcodeValue);
				const auto Type = static_cast<EMaterialProgramValueType>(TypeValue);
				if (GetOpcodeDescriptor(Opcode).Purpose != EMaterialGraphOpcodePurpose::Authored) continue;
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
			const auto& Descriptor = GetOpcodeDescriptor(Entry.Opcode);
			if (Descriptor.Purpose != EMaterialGraphOpcodePurpose::Authored) continue;
			switch (Descriptor.PaletteShape)
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
					Descriptor.PaletteShape == EMaterialGraphPaletteShape::AdaptiveVector
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
