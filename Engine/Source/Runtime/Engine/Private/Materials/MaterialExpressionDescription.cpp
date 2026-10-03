#include "Materials/MaterialExpressionDescription.h"
#include "Materials/MaterialExpressions.h"
#include "DObject/Class.h"
#include "Misc/AssertionMacros.h"

namespace Durin
{
	namespace
	{
		template<typename Expression>
		auto Describe(EMaterialProgramOpcode Opcode, std::string_view Name,
			std::string_view Description, std::array<const char*, 8> InputNames = {},
			std::string_view Keywords = {}, std::optional<EMaterialProgramValueType> FixedType = {})
			-> FMaterialExpressionDescription
		{
			static_assert(std::is_base_of_v<DMaterialExpression, Expression>);
			using Behavior = EMaterialExpressionOutputBehavior;
			const bool bInstance = Opcode == EMaterialProgramOpcode::FunctionInput
				|| Opcode == EMaterialProgramOpcode::FunctionOutput || Opcode == EMaterialProgramOpcode::FunctionCall
				|| Opcode == EMaterialProgramOpcode::CollectionParameter || Opcode == EMaterialProgramOpcode::Swizzle
				|| Opcode == EMaterialProgramOpcode::AppendVector || Opcode == EMaterialProgramOpcode::GetSurfaceAttributes
				|| Opcode == EMaterialProgramOpcode::SetSurfaceAttributes;
			FMaterialExpressionDescription Result{Expression::StaticClass(), Opcode, Name, Description, Keywords,
				bInstance ? Behavior::Instance : IsMaterialAdaptiveNumeric(Opcode) ? Behavior::AdaptiveNumeric : Behavior::Fixed, {}};
			for (uint8 TypeValue = 0; TypeValue <= static_cast<uint8>(EMaterialProgramValueType::StaticBool); ++TypeValue)
			{
				const auto Type = static_cast<EMaterialProgramValueType>(TypeValue);
				if (FixedType && Type != *FixedType) continue;
				if constexpr (std::is_same_v<Expression, DMaterialExpressionVector4Parameter>)
					if (Type == EMaterialProgramValueType::Float) continue;
				const auto Signature = GetMaterialProgramNodeSignature(Opcode, Type);
				if (!Signature) continue;
				FMaterialExpressionAuthoringShape Shape{Type, {}, {}};
				for (uint8 Index = 0; Index < Signature->InputCount; ++Index)
				{
					Shape.InputNames.emplace_back(InputNames[Index] ? InputNames[Index] : "Value");
					Shape.AcceptedInputTypes.emplace_back(Signature->Inputs[Index].begin(), Signature->Inputs[Index].end());
					const bool bExactNormal = Opcode == EMaterialProgramOpcode::BlendNormalsRNM
						|| (Opcode == EMaterialProgramOpcode::MakeSurface && Index == static_cast<uint8>(EMaterialSurfaceOutput::Normal));
					if (MaterialNumericInputAllowsScalarBroadcast(Opcode, Index) && !bExactNormal
						&& Signature->Inputs[Index].size() == 1 && Signature->Inputs[Index].front() > EMaterialProgramValueType::Float
						&& Signature->Inputs[Index].front() <= EMaterialProgramValueType::Float4)
						Shape.AcceptedInputTypes.back().push_back(EMaterialProgramValueType::Float);
				}
				Result.Shapes.push_back(std::move(Shape));
			}
			return Result;
		}
	}

	auto GetMaterialExpressionDescriptions() -> std::span<const FMaterialExpressionDescription>
	{
		// Reflection must be initialized before the first call. No probe objects are created.
		static const std::vector<FMaterialExpressionDescription> Descriptions = [] {
			std::vector<FMaterialExpressionDescription> Result{
				Describe<DMaterialExpressionScalarConstant>(EMaterialProgramOpcode::Constant, "Constant",
					"A literal numeric value. Choose Float, Float2, Float3, or Float4 from the node type menu.", {}, "", EMaterialProgramValueType::Float),
				Describe<DMaterialExpressionVector2Constant>(EMaterialProgramOpcode::Constant, "Constant",
					"A literal numeric value. Choose Float, Float2, Float3, or Float4 from the node type menu.", {}, "", EMaterialProgramValueType::Float2),
				Describe<DMaterialExpressionVector3Constant>(EMaterialProgramOpcode::Constant, "Constant",
					"A literal numeric value. Choose Float, Float2, Float3, or Float4 from the node type menu.", {}, "", EMaterialProgramValueType::Float3),
				Describe<DMaterialExpressionVector4Constant>(EMaterialProgramOpcode::Constant, "Constant",
					"A literal numeric value. Choose Float, Float2, Float3, or Float4 from the node type menu.", {}, "", EMaterialProgramValueType::Float4),
				Describe<DMaterialExpressionScalarParameter>(EMaterialProgramOpcode::Parameter, "Parameter",
					"A value exposed by the material parameter definition.", {}, "", EMaterialProgramValueType::Float),
				Describe<DMaterialExpressionVector4Parameter>(EMaterialProgramOpcode::Parameter, "Parameter",
					"A value exposed by the material parameter definition.", {}, ""),
				Describe<DMaterialExpressionTextureParameter>(EMaterialProgramOpcode::TextureParameter, "Texture Object Parameter",
					"A texture resource for function inputs or multiple samples. For ordinary texture mapping, use Texture Sample Parameter 2D.", {}, ""),
				Describe<DMaterialExpressionTextureSampleParameter2D>(EMaterialProgramOpcode::TextureSampleParameter2D, "Texture Sample Parameter 2D",
					"Samples a named texture parameter with mesh UV0 or a connected Float2 UV expression. Outputs share one fetch.", {"UV"}, ""),
				Describe<DMaterialExpressionWorldPosition>(EMaterialProgramOpcode::WorldPosition, "World Position",
					"Surface position relative to the current view origin (Float3).", {}, ""),
				Describe<DMaterialExpressionTime>(EMaterialProgramOpcode::Time, "Time",
					"Elapsed real time in seconds (Float), updated every rendered view.", {}, ""),
				Describe<DMaterialExpressionCollectionParameter>(EMaterialProgramOpcode::CollectionParameter, "Collection Parameter",
					"Reads a numeric value from a material parameter collection in the current world.", {}, ""),
				Describe<DMaterialExpressionCameraPosition>(EMaterialProgramOpcode::CameraPosition, "Camera Position",
					"Active pass camera position in translated world space; zero at the view origin (Float3).", {}, ""),
				Describe<DMaterialExpressionCameraVector>(EMaterialProgramOpcode::CameraVector, "Camera Vector",
					"Direction from the fragment to the active pass camera (Float3).", {}, ""),
				Describe<DMaterialExpressionObjectPosition>(EMaterialProgramOpcode::ObjectPosition, "Object Position",
					"Render primitive bounds center relative to the current view origin (Float3).", {}, ""),
				Describe<DMaterialExpressionVertexInterpolator>(EMaterialProgramOpcode::VertexInterpolator, "Vertex Interpolator",
					"Evaluates the input per vertex and interpolates its value to pixel calculations.", {}, ""),
				Describe<DMaterialExpressionVertexNormal>(EMaterialProgramOpcode::VertexNormal, "Vertex Normal",
					"Post-vertex-factory world normal for vertex offsets or explicit interpolation.", {}, ""),
				Describe<DMaterialExpressionScreenPosition>(EMaterialProgramOpcode::ScreenPosition, "Screen Position",
					"Normalized position within the active pass viewport (Float2).", {}, ""),
				Describe<DMaterialExpressionViewSize>(EMaterialProgramOpcode::ViewSize, "View Size",
					"Active pass viewport size in pixels (Float2).", {}, ""),
				Describe<DMaterialExpressionStaticBool>(EMaterialProgramOpcode::StaticBool, "Static Bool",
					"Declares a root-owned compile-time boolean keyed by stable GUID.", {}, ""),
				Describe<DMaterialExpressionStaticSwitch>(EMaterialProgramOpcode::StaticSwitch, "Static Switch",
					"Selects exactly one branch from a static bool before normalized MIR.", {"Condition", "False", "True"}, ""),
				Describe<DMaterialExpressionQualitySwitch>(EMaterialProgramOpcode::QualitySwitch, "Quality Switch",
					"Selects Low or High, using Default when that branch is unconnected.", {"Default", "Low", "High"}, ""),
				Describe<DMaterialExpressionFeatureLevelSwitch>(EMaterialProgramOpcode::FeatureLevelSwitch, "Feature Level Switch",
					"Selects the accepted RHI feature tier, using Default when unconnected.", {"Default", "ES3_1", "SM5", "SM6"}, ""),
				Describe<DMaterialExpressionTransformPosition>(EMaterialProgramOpcode::TransformPosition, "Transform Position",
					"Transforms a spatial position between explicit coordinate spaces; World is relative to the current view origin.", {"Input"}, ""),
				Describe<DMaterialExpressionTransformDirection>(EMaterialProgramOpcode::TransformDirection, "Transform Direction",
					"Transforms a spatial direction between explicit coordinate spaces.", {"Input"}, ""),
				Describe<DMaterialExpressionTransformNormal>(EMaterialProgramOpcode::TransformNormal, "Transform Normal",
					"Transforms and normalizes a spatial normal with inverse-transpose semantics.", {"Input"}, ""),
				Describe<DMaterialExpressionTextureCoordinates>(EMaterialProgramOpcode::TextureCoordinates, "Texture Coordinates",
					"Reads a mesh UV channel as Float2. Apply transforms with upstream math nodes.", {"Channel"}, "UV Channel UVChannel"),
				Describe<DMaterialExpressionTextureSample2D>(EMaterialProgramOpcode::TextureSample2D, "Texture Sample 2D",
					"Samples a connected texture resource. For a standalone replaceable texture, use Texture Sample Parameter 2D.", {"Texture", "UV"}, ""),
				Describe<DMaterialExpressionAdd>(EMaterialProgramOpcode::Add, "Add",
					"Adds two values component by component.", {"A", "B"}, ""),
				Describe<DMaterialExpressionSubtract>(EMaterialProgramOpcode::Subtract, "Subtract",
					"Subtracts the second value from the first.", {"A", "B"}, ""),
				Describe<DMaterialExpressionMultiply>(EMaterialProgramOpcode::Multiply, "Multiply",
					"Multiplies two values component by component.", {"A", "B"}, ""),
				Describe<DMaterialExpressionDivide>(EMaterialProgramOpcode::Divide, "Divide",
					"Divides the first value by the second.", {"A", "B"}, ""),
				Describe<DMaterialExpressionMinimum>(EMaterialProgramOpcode::Minimum, "Minimum",
					"Returns the component-wise minimum.", {"A", "B"}, ""),
				Describe<DMaterialExpressionMaximum>(EMaterialProgramOpcode::Maximum, "Maximum",
					"Returns the component-wise maximum.", {"A", "B"}, ""),
				Describe<DMaterialExpressionNegate>(EMaterialProgramOpcode::Negate, "Negate",
					"Reverses the sign of a value.", {}, ""),
				Describe<DMaterialExpressionOneMinus>(EMaterialProgramOpcode::OneMinus, "One Minus",
					"Subtracts a value from one.", {}, ""),
				Describe<DMaterialExpressionAbsolute>(EMaterialProgramOpcode::Absolute, "Absolute",
					"Returns the absolute value.", {}, ""),
				Describe<DMaterialExpressionSaturate>(EMaterialProgramOpcode::Saturate, "Saturate",
					"Clamps a value to the zero-to-one range.", {}, ""),
				Describe<DMaterialExpressionNormalize>(EMaterialProgramOpcode::Normalize, "Normalize",
					"Returns a unit-length vector.", {}, ""),
				Describe<DMaterialExpressionClamp>(EMaterialProgramOpcode::Clamp, "Clamp",
					"Constrains a value between minimum and maximum inputs.", {"Value", "Min", "Max"}, ""),
				Describe<DMaterialExpressionLerp>(EMaterialProgramOpcode::Lerp, "Lerp",
					"Interpolates between two values.", {"A", "B", "Alpha"}, ""),
				Describe<DMaterialExpressionMakeVector2>(EMaterialProgramOpcode::MakeFloat2, "Make Vector",
					"Combines scalar inputs into a vector.", {"X", "Y"}, ""),
				Describe<DMaterialExpressionMakeVector3>(EMaterialProgramOpcode::MakeFloat3, "Make Vector",
					"Combines scalar inputs into a vector.", {"X", "Y", "Z"}, ""),
				Describe<DMaterialExpressionMakeVector4>(EMaterialProgramOpcode::MakeFloat4, "Make Vector",
					"Combines scalar inputs into a vector.", {"X", "Y", "Z", "W"}, ""),
				Describe<DMaterialExpressionAppendVector>(EMaterialProgramOpcode::AppendVector, "Append Vector",
					"Concatenates A and B; output width follows the inputs (up to four components).", {"A", "B"}, ""),
				Describe<DMaterialExpressionSwizzle>(EMaterialProgramOpcode::Swizzle, "Component Mask",
					"Selects, repeats or reorders channels (Component Mask / Truncate).", {}, ""),
				Describe<DMaterialExpressionSplat2>(EMaterialProgramOpcode::Splat2, "Splat",
					"Replicates a scalar across vector components.", {}, ""),
				Describe<DMaterialExpressionSplat3>(EMaterialProgramOpcode::Splat3, "Splat",
					"Replicates a scalar across vector components.", {}, ""),
				Describe<DMaterialExpressionSplat4>(EMaterialProgramOpcode::Splat4, "Splat",
					"Replicates a scalar across vector components.", {}, ""),
				Describe<DMaterialExpressionBlendNormalsRNM>(EMaterialProgramOpcode::BlendNormalsRNM, "Blend Normals RNM",
					"Blends two tangent-space normals with RNM.", {"Base", "Detail"}, ""),
				Describe<DMaterialExpressionSine>(EMaterialProgramOpcode::Sine, "Sine",
					"Returns the component-wise sine in radians.", {}, ""),
				Describe<DMaterialExpressionCosine>(EMaterialProgramOpcode::Cosine, "Cosine",
					"Returns the component-wise cosine in radians.", {}, ""),
				Describe<DMaterialExpressionDot>(EMaterialProgramOpcode::Dot, "Dot Product",
					"Returns the scalar dot product of equal-width vectors.", {"A", "B"}, ""),
				Describe<DMaterialExpressionCross>(EMaterialProgramOpcode::Cross, "Cross Product",
					"Returns the cross product of two Float3 values.", {"A", "B"}, ""),
				Describe<DMaterialExpressionLength>(EMaterialProgramOpcode::Length, "Length",
					"Returns the scalar length of a vector.", {}, ""),
				Describe<DMaterialExpressionDistance>(EMaterialProgramOpcode::Distance, "Distance",
					"Returns the scalar distance between equal-width values.", {"A", "B"}, ""),
				Describe<DMaterialExpressionPow>(EMaterialProgramOpcode::Pow, "Power",
					"Raises each base component to its exponent.", {"Base", "Exponent"}, ""),
				Describe<DMaterialExpressionSqrt>(EMaterialProgramOpcode::Sqrt, "Square Root",
					"Returns the component-wise square root.", {}, ""),
				Describe<DMaterialExpressionExp>(EMaterialProgramOpcode::Exp, "Exponential",
					"Returns the component-wise natural exponential.", {}, ""),
				Describe<DMaterialExpressionLog>(EMaterialProgramOpcode::Log, "Natural Log",
					"Returns the component-wise natural logarithm.", {}, ""),
				Describe<DMaterialExpressionFloor>(EMaterialProgramOpcode::Floor, "Floor",
					"Rounds each component down.", {}, ""),
				Describe<DMaterialExpressionCeil>(EMaterialProgramOpcode::Ceil, "Ceil",
					"Rounds each component up.", {}, ""),
				Describe<DMaterialExpressionRound>(EMaterialProgramOpcode::Round, "Round",
					"Rounds each component to the nearest integer.", {}, ""),
				Describe<DMaterialExpressionFrac>(EMaterialProgramOpcode::Frac, "Fraction",
					"Returns each component's fractional part.", {}, ""),
				Describe<DMaterialExpressionFmod>(EMaterialProgramOpcode::Fmod, "Fmod",
					"Returns the component-wise floating-point remainder.", {"A", "B"}, ""),
				Describe<DMaterialExpressionStep>(EMaterialProgramOpcode::Step, "Step",
					"Returns zero below Edge and one otherwise.", {"Edge", "Value"}, ""),
				Describe<DMaterialExpressionSmoothStep>(EMaterialProgramOpcode::SmoothStep, "Smooth Step",
					"Returns smooth Hermite interpolation between Min and Max.", {"Min", "Max", "Value"}, ""),
				Describe<DMaterialExpressionSign>(EMaterialProgramOpcode::Sign, "Sign",
					"Returns the sign of each component.", {}, ""),
				Describe<DMaterialExpressionReflect>(EMaterialProgramOpcode::Reflect, "Reflect",
					"Reflects an incident vector around a normal of equal width.", {"Incident", "Normal"}, ""),
				Describe<DMaterialExpressionMakeSurface>(EMaterialProgramOpcode::MakeSurface, "Make Surface",
					"Combines eight explicit surface properties without hidden parameter access.", {"Base Color", "Normal", "Metallic", "Roughness", "Ambient Occlusion", "Emissive", "Opacity", "Opacity Mask"}, ""),
				Describe<DMaterialExpressionFunctionInput>(EMaterialProgramOpcode::FunctionInput, "Function Input",
					"Reads an input from the function signature.", {}, ""),
				Describe<DMaterialExpressionFunctionOutput>(EMaterialProgramOpcode::FunctionOutput, "Function Output",
					"Publishes a value through the function signature.", {}, ""),
				Describe<DMaterialExpressionFunctionCall>(EMaterialProgramOpcode::FunctionCall, "Function Call",
					"Evaluates a material function with its bound inputs.", {}, ""),
				Describe<DMaterialExpressionGetSurfaceAttributes>(EMaterialProgramOpcode::GetSurfaceAttributes, "Get Surface Attributes",
					"Reads selected attributes from a Surface.", {"Surface"}, ""),
				Describe<DMaterialExpressionSetSurfaceAttributes>(EMaterialProgramOpcode::SetSurfaceAttributes, "Set Surface Attributes",
					"Overrides selected attributes while retaining the base Surface.", {"Surface"}, ""),
				{DMaterialExpressionMaterialOutput::StaticClass(), std::nullopt, "Material Output",
					"Publishes material domain outputs.", {}, EMaterialExpressionOutputBehavior::Instance, {}},
			};
			std::unordered_map<const DClass*, bool> Classes;
			for (const auto& Description : Result)
				requiref(Description.ExpressionClass && Classes.emplace(Description.ExpressionClass, true).second,
					"Material expression descriptions require unique reflected classes.");
			return Result;
		}();
		return Descriptions;
	}

	auto FindMaterialExpressionDescription(const DClass* Class) -> const FMaterialExpressionDescription*
	{
		static const auto ByClass = [] {
			std::unordered_map<const DClass*, const FMaterialExpressionDescription*> Result;
			for (const auto& Description : GetMaterialExpressionDescriptions())
				Result.emplace(Description.ExpressionClass, &Description);
			return Result;
		}();
		const auto It = ByClass.find(Class);
		return It == ByClass.end() ? nullptr : It->second;
	}
}
