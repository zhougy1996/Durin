#include "MaterialPreparedProgram.h"
#include "Materials/MaterialProgramCompiler.h"

#include "Materials/MaterialTypes.h"
#include "Materials/MaterialRenderTypes.h"

#include <array>
#include <chrono>
#include <format>
#include <ranges>
#include <set>

namespace Durin
{
	namespace
	{
		auto SlangType(EMaterialProgramValueType Type) -> std::string_view
		{
			switch (Type)
			{
			case EMaterialProgramValueType::Float: return "float";
			case EMaterialProgramValueType::Float2: return "float2";
			case EMaterialProgramValueType::Float3: return "float3";
			case EMaterialProgramValueType::Float4: return "float4";
			case EMaterialProgramValueType::Texture2D:
				return "Texture2D<float4>";
			case EMaterialProgramValueType::Surface: return "FMaterialSurface";
			}
			return {};
		}

		auto FloatExpression(float Value) -> std::string
		{
			const uint32 Bits = Value == 0.0f
				? 0u : std::bit_cast<uint32>(Value);
			return std::format("asfloat(0x{:08x}u)", Bits);
		}

		auto LiteralExpression(const MIR::FNode& Node) -> std::string
		{
			std::array Values{Node.GetLiteral().X, Node.GetLiteral().Y,
				Node.GetLiteral().Z, Node.GetLiteral().W};
			const uint32 Width = static_cast<uint32>(Node.ResultType) + 1;
			if (Width == 1) return FloatExpression(Values[0]);
			std::string Result = std::format("{}(", SlangType(Node.ResultType));
			for (uint32 Index = 0; Index < Width; ++Index)
			{
				if (Index) Result += ", ";
				Result += FloatExpression(Values[Index]);
			}
			return Result + ")";
		}

		auto LiteralExpression(EMaterialProgramValueType Type,
			const FMaterialProgramLiteral& Literal) -> std::string
		{
			MIR::FNode Node;
			Node.ResultType = Type;
			Node.Payload = Literal;
			return LiteralExpression(Node);
		}

		auto MakeDiagnostic(EMaterialProgramDiagnosticCategory Category,
			FMaterialError Error) -> FMaterialProgramDiagnostic
		{
			return {.Category = Category, .Error = std::move(Error)};
		}
	}

	static auto GenerateMaterialProgramSlangImpl(const MIR::FModule& IR,
		const FMaterialRenderLayout& Layout, std::string& OutSource) -> FMaterialOperationResult
	{
		OutSource.clear();
		if (IR.Version != MIR::CurrentVersion
			|| IR.Nodes.size() > MaterialFunctionMaxExpandedNodes)
		{
			return {EMaterialIRError::InvalidMaterialIRSlangGeneration};
		}
		OutSource = R"(module DurinGeneratedMaterial;
import Material.SurfaceMaterial;
import Material.SpecularAntialiasing;
import Lighting.DirectionalShadow;
import Lighting.ForwardLightingUniform;
import Lighting.SurfaceLighting;

struct VSOutput
{
    float4 pos : SV_Position;
    float4 color : COLOR;
    float3 worldPosition : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float4 worldTangent : TEXCOORD2;
    float2 uv0 : TEXCOORD3;
    float2 uv1 : TEXCOORD4;
    float2 uv2 : TEXCOORD5;
    float2 uv3 : TEXCOORD6;
};
)";
		OutSource += "struct MaterialUniform\n{\n    // Reserved header; view/pass controls live in descriptor set 0.\n    float4 Reserved;\n";
		for (uint32 Index = 0; Index < Layout.UniformFieldCount; ++Index)
			OutSource += std::format("    float4 Value{};\n", Index);
		OutSource += "};\n[[vk::binding(1, 0)]] ConstantBuffer<FForwardLightingUniform> Lighting;\n"
			"[[vk::binding(2, 1)]] ConstantBuffer<MaterialUniform> Material;\nstruct MeshViewUniform { float4 Parameters; };\n[[vk::binding(0, 0)]] ConstantBuffer<MeshViewUniform> MeshView;\n";
		for (uint32 Index = 0; Index < Layout.ResourceFieldCount; ++Index)
			OutSource += std::format(
				"[[vk::binding({}, 1)]] Texture2D<float4> MaterialTexture{};\n"
				"[[vk::binding({}, 1)]] SamplerState MaterialSampler{};\n",
				MaterialTextureBindingBase + 2 * Index, Index,
				MaterialTextureBindingBase + 2 * Index + 1, Index);
		OutSource += R"(
[[vk::binding(19, 0)]] TextureCube<float4> EnvironmentIrradiance;
[[vk::binding(20, 0)]] TextureCube<float4> EnvironmentPrefiltered;
[[vk::binding(21, 0)]] Texture2D<float4> EnvironmentBrdfLut;
[[vk::binding(22, 0)]] SamplerState EnvironmentSampler;
[[vk::binding(25, 0)]] Texture2DArray<float> DirectionalShadowTexture;
[[vk::binding(26, 0)]] SamplerComparisonState DirectionalShadowSampler;
)";
		auto FindField = [&](const FGuid& Id) -> const FMaterialRenderField* {
			const auto It = std::ranges::find(Layout.Fields, Id, &FMaterialRenderField::ParameterId);
			return It == Layout.Fields.end() ? nullptr : &*It;
		};
		OutSource += R"(
float2 SelectAuthoredUV(VSOutput input, float channel)
{
    uint index = (uint)clamp(floor(channel + 0.5), 0.0, 3.0);
    return index == 1u ? input.uv1 : (index == 2u ? input.uv2 : (index == 3u ? input.uv3 : input.uv0));
}
FMaterialSurface MakeAuthoredSurface(float3 baseColor, float3 normal, float metallic,
    float roughness, float ao, float3 emissive, float opacity, float mask)
{
    FMaterialSurface s;
    s.baseColor = baseColor; s.tangentNormal = normal; s.metallic = metallic;
    s.roughness = roughness; s.ambientOcclusion = ao; s.emissive = emissive;
    s.opacity = opacity; s.opacityMask = mask;
    return s;
}
)";
		OutSource += R"(
FMaterialSurface EvaluateGeneratedMaterial(VSOutput input)
{
)";

		std::vector<std::string> Expressions(IR.Nodes.size());
		for (uint32 Index = 0; Index < IR.Nodes.size(); ++Index)
		{
			const MIR::FNode& Node = IR.Nodes[Index];
			auto Input = [&](size_t Slot) -> const std::string& {
				return Expressions[Node.Inputs[Slot]];
			};
			std::string Expression;
			switch (Node.Opcode)
			{
			case EMaterialProgramOpcode::FunctionInput:
			case EMaterialProgramOpcode::FunctionOutput:
			case EMaterialProgramOpcode::FunctionCall:
			case EMaterialProgramOpcode::TextureSampleParameter2D:
			case EMaterialProgramOpcode::TextureCoordinates:
			case EMaterialProgramOpcode::GetSurfaceAttributes:
			case EMaterialProgramOpcode::SetSurfaceAttributes:
			case EMaterialProgramOpcode::AppendVector:
				// Authored operations must be expanded before source generation.
				break;
			case EMaterialProgramOpcode::WorldPosition: Expression = "input.worldPosition"; break;
			case EMaterialProgramOpcode::Time: Expression = "MeshView.Parameters.x"; break;
			case EMaterialProgramOpcode::UVChannel: Expression = std::format("SelectAuthoredUV(input, {})", Input(0)); break;
			case EMaterialProgramOpcode::Sine: Expression = std::format("sin({})", Input(0)); break;
			case EMaterialProgramOpcode::Cosine: Expression = std::format("cos({})", Input(0)); break;
			case EMaterialProgramOpcode::MakeSurface:
				Expression = std::format("MakeAuthoredSurface({}, {}, {}, {}, {}, {}, {}, {})",
					Input(0), Input(1), Input(2), Input(3), Input(4), Input(5), Input(6), Input(7)); break;
			case EMaterialProgramOpcode::Constant:
				Expression = LiteralExpression(Node); break;
			case EMaterialProgramOpcode::Parameter:
				if (const auto* Field = FindField(Node.GetParameterId()))
				{
					constexpr std::array<std::string_view, 4> Swizzles{".x", ".xy", ".xyz", ""};
					Expression = std::format("Material.Value{}{}", Field->CompactIndex,
						Swizzles[static_cast<size_t>(Node.ResultType)]);
				}
				break;
			case EMaterialProgramOpcode::TextureParameter:
			{
				if (const auto* Field = FindField(Node.GetParameterId()))
					Expression = std::format("MaterialTexture{}", Field->CompactIndex);
				break;
			}
			case EMaterialProgramOpcode::TextureSample2D:
			{
				const auto& TextureNode = IR.Nodes[Node.Inputs[0]];
				if (const auto* Field = FindField(TextureNode.GetParameterId()))
					Expression = std::format("{}.Sample(MaterialSampler{}, {})",
						Input(0), Field->CompactIndex, Input(1));
				break;
			}
			case EMaterialProgramOpcode::Add: Expression = std::format("({} + {})", Input(0), Input(1)); break;
			case EMaterialProgramOpcode::Subtract: Expression = std::format("({} - {})", Input(0), Input(1)); break;
			case EMaterialProgramOpcode::Multiply: Expression = std::format("({} * {})", Input(0), Input(1)); break;
			case EMaterialProgramOpcode::Divide: Expression = std::format("({} / {})", Input(0), Input(1)); break;
			case EMaterialProgramOpcode::Minimum: Expression = std::format("min({}, {})", Input(0), Input(1)); break;
			case EMaterialProgramOpcode::Maximum: Expression = std::format("max({}, {})", Input(0), Input(1)); break;
			case EMaterialProgramOpcode::Negate: Expression = std::format("(-{})", Input(0)); break;
			case EMaterialProgramOpcode::OneMinus: Expression = std::format("(1.0 - {})", Input(0)); break;
			case EMaterialProgramOpcode::Absolute: Expression = std::format("abs({})", Input(0)); break;
			case EMaterialProgramOpcode::Saturate: Expression = std::format("saturate({})", Input(0)); break;
			case EMaterialProgramOpcode::Normalize: Expression = std::format("normalize({})", Input(0)); break;
			case EMaterialProgramOpcode::Clamp: Expression = std::format("clamp({}, {}, {})", Input(0), Input(1), Input(2)); break;
			case EMaterialProgramOpcode::Lerp: Expression = std::format("lerp({}, {}, {})", Input(0), Input(1), Input(2)); break;
			case EMaterialProgramOpcode::MakeFloat2:
			case EMaterialProgramOpcode::MakeFloat3:
			case EMaterialProgramOpcode::MakeFloat4:
			{
				Expression = std::format("{}(", SlangType(Node.ResultType));
				for (size_t Slot = 0; Slot < Node.Inputs.size(); ++Slot)
				{
					if (Slot) Expression += ", ";
					Expression += Input(Slot);
				}
				Expression += ")"; break;
			}
			case EMaterialProgramOpcode::Swizzle:
			{
				constexpr std::string_view Components = "xyzw";
				std::array Mask{Node.GetSwizzle().Components[0], Node.GetSwizzle().Components[1],
					Node.GetSwizzle().Components[2], Node.GetSwizzle().Components[3]};
				Expression = Input(0) + ".";
				for (uint8 Slot = 0; Slot < Node.GetSwizzle().Length; ++Slot)
					Expression += Components[Mask[Slot]];
				break;
			}
			case EMaterialProgramOpcode::Splat2:
			case EMaterialProgramOpcode::Splat3:
			case EMaterialProgramOpcode::Splat4:
				Expression = std::format("{}({})", SlangType(Node.ResultType), Input(0)); break;
			case EMaterialProgramOpcode::DecodeNormalRG: Expression = std::format("DecodeTextureNormal({})", Input(0)); break;
			case EMaterialProgramOpcode::BlendNormalsRNM: Expression = std::format("BlendSurfaceNormalsRNM({}, {})", Input(0), Input(1)); break;
			}
			if (Expression.empty())
			{
				OutSource.clear();
				return {FMaterialError(EMaterialIRError::UnsupportedGenerationNode, Index)};
			}
			Expressions[Index] = std::format("n{}", Index);
			if (Node.ResultType != EMaterialProgramValueType::Texture2D)
				OutSource += std::format("    {} n{} = {};\n",
					SlangType(Node.ResultType), Index, Expression);
			else
				Expressions[Index] = std::move(Expression);
		}
		if (IR.SurfaceRoot.bAggregate)
		{
			if (IR.SurfaceRoot.AggregateExpressionIndex >= Expressions.size())
			{
				OutSource.clear();
				return {EMaterialIRError::AggregateSurfaceRootExpressionOutBounds};
			}
			OutSource += std::format("    return EvaluateMaterialSurface({});\n}}\n", Expressions[IR.SurfaceRoot.AggregateExpressionIndex]);
		}
		else
		{
			std::array<std::string, 8> Outputs;
			for (size_t Index = 0; Index < Outputs.size(); ++Index)
			{
				const auto& Input = IR.SurfaceRoot.Inputs[Index];
				if (Input.bExpression)
				{
					if (Input.ExpressionIndex >= Expressions.size())
					{
						OutSource.clear();
						return {EMaterialIRError::PerPropertySurfaceRootExpressionOutBounds};
					}
					Outputs[Index] = Expressions[Input.ExpressionIndex];
				}
				else Outputs[Index] = LiteralExpression(Input.Type, Input.Literal);
			}
			OutSource += std::format(R"(    FMaterialSurface result;
    result.baseColor = {};
    result.tangentNormal = {};
    result.metallic = {};
    result.roughness = {};
    result.ambientOcclusion = {};
    result.emissive = {};
    result.opacity = {};
    result.opacityMask = {};
    return EvaluateMaterialSurface(result);
}}
)", Outputs[0], Outputs[1], Outputs[2], Outputs[3], Outputs[4], Outputs[5],
			Outputs[6], Outputs[7]);
		}
		OutSource += R"(
struct FResolvedGeneratedSurfaceShading
{
    FMaterialNormalFrame normalFrame;
    float effectiveRoughness;
};
FResolvedGeneratedSurfaceShading ResolveGeneratedSurfaceShading(
    VSOutput input, FMaterialSurface surface, bool isFrontFace)
{
    FResolvedGeneratedSurfaceShading result;
    result.normalFrame = EvaluateMaterialNormalFrame(input.worldNormal,
        input.worldTangent, surface.tangentNormal, isFrontFace);
    result.effectiveRoughness = FilterSpecularRoughness(surface.roughness,
        result.normalFrame.shadingNormal, MeshView.Parameters.w > 0.5);
    return result;
}
struct GeometryPassFragmentOutput
{
    float4 material : SV_Target0;
    float4 normals : SV_Target1;
    float4 surface : SV_Target2;
    float4 emissive : SV_Target3;
};
[shader("fragment")]
GeometryPassFragmentOutput GeometryFragmentMain(
    VSOutput input, bool isFrontFace : SV_IsFrontFace)
{
#if DURIN_MATERIAL_SHADING_MODEL == 1
    discard;
    GeometryPassFragmentOutput empty = (GeometryPassFragmentOutput)0;
    return empty;
#else
    FMaterialSurface s = EvaluateGeneratedMaterial(input);
    FResolvedGeneratedSurfaceShading shading =
        ResolveGeneratedSurfaceShading(input, s, isFrontFace);
#if DURIN_MATERIAL_BLEND_MODE == 1
    if (RejectMaterialMask(s.opacityMask, asfloat(uint(
            DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS))))
        discard;
#endif
    GeometryPassFragmentOutput o;
    o.material = float4(s.baseColor, s.metallic);
    o.normals = float4(
        EncodeOctahedralNormal(shading.normalFrame.shadingNormal),
        EncodeOctahedralNormal(shading.normalFrame.geometricNormal));
    o.surface = float4(shading.effectiveRoughness,
        s.ambientOcclusion, s.opacity, 1.0 / 255.0);
    o.emissive = float4(s.emissive, 0.0);
    return o;
#endif
}
struct HitProxyUniform { uint4 Id; float4 ViewOrigin; };
[[vk::binding(27, 1)]] ConstantBuffer<HitProxyUniform> HitProxy;
[shader("fragment")]
uint2 HitProxyFragmentMain(VSOutput input) : SV_Target0
{
#if DURIN_MATERIAL_BLEND_MODE == 1
    float mask = DURIN_GENERATED_SHADOW_MASK;
    if (mask < asfloat(uint(DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS))) discard;
#endif
    return uint2(HitProxy.Id.x, asuint(length(input.worldPosition - HitProxy.ViewOrigin.xyz)));
}
[shader("fragment")]
void ShadowFragmentMain(VSOutput input)
{
#if DURIN_MATERIAL_BLEND_MODE == 1
        float mask = DURIN_GENERATED_SHADOW_MASK;
    if (mask < asfloat(uint(DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS)))
        discard;
#endif
}
[shader("fragment")]
float4 FragmentMain(
    VSOutput input, bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
    FMaterialSurface s = EvaluateGeneratedMaterial(input);
    FResolvedGeneratedSurfaceShading shading;
#if DURIN_MATERIAL_SHADING_MODEL == 0
    shading = ResolveGeneratedSurfaceShading(input, s, isFrontFace);
#endif
#if DURIN_MATERIAL_BLEND_MODE == 1
    if (RejectMaterialMask(s.opacityMask, asfloat(uint(
            DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS))))
        discard;
#endif
#if DURIN_MATERIAL_SHADING_MODEL == 1
    return float4(s.baseColor + s.emissive, s.opacity);
#else
    if (MeshView.Parameters.z < 0.5) return float4(s.baseColor + s.emissive, s.opacity);
    FSurfaceLightingFrame lightingFrame = BuildSurfaceLightingFrame(
        input.worldPosition, shading.normalFrame.shadingNormal,
        Lighting.ViewPosition.xyz);
    float3 direct = float3(0.0);
    if (Lighting.Counts.x > 0u)
    {
        float3 toLight = SafeSurfaceNormal(-Lighting.DirectionalDirection.xyz);
        float3 radiance = Lighting.DirectionalColorIntensity.rgb
            * Lighting.DirectionalColorIntensity.a;
        FDirectionalShadowSample shadow = EvaluateDirectionalShadow(
            input.worldPosition, shading.normalFrame.geometricNormal,
            Lighting.DirectionalShadowControl,
            Lighting.DirectionalShadowViewDepthTransform,
            Lighting.DirectionalShadowSplitDepths,
            Lighting.DirectionalShadowLightTransition,
            Lighting.DirectionalShadowCascades,
            DirectionalShadowTexture, DirectionalShadowSampler);
        if (Lighting.DirectionalShadowControl.y > 0.5)
            return float4(shadow.diagnosticColor, s.opacity);
        direct += EvaluateSurfaceDirectionalLighting(s.baseColor, s.metallic,
            shading.effectiveRoughness, lightingFrame, toLight, radiance,
            shadow.attenuation);
    }
    [unroll]
    for (uint lightIndex = 0u; lightIndex < 4u; ++lightIndex)
    {
        if (lightIndex >= Lighting.Counts.y)
            break;
        direct += EvaluateSurfaceLocalLighting(s.baseColor, s.metallic,
            shading.effectiveRoughness, input.worldPosition, lightingFrame,
            Lighting.Local[lightIndex]);
    }
    float3 environment = EvaluateSurfaceEnvironmentLighting(s.baseColor,
        s.metallic, shading.effectiveRoughness, s.ambientOcclusion,
        lightingFrame, EnvironmentIrradiance, EnvironmentPrefiltered,
        EnvironmentBrdfLut, EnvironmentSampler, Lighting.EnvironmentRotation, Lighting.EnvironmentControl);
    return ComposeSurfaceLighting(direct, environment, s.emissive, s.opacity);
#endif
}
)";
		const std::string_view MaskToken = "DURIN_GENERATED_SHADOW_MASK";
		for (auto Position = OutSource.find(MaskToken); Position != std::string::npos; Position = OutSource.find(MaskToken))
			OutSource.replace(Position, MaskToken.size(), "EvaluateGeneratedMaterial(input).opacityMask");
		if (OutSource.size() > MaterialProgramMaxCanonicalBytes)
		{
			OutSource.clear();
			return {EMaterialIRError::GeneratedMaterialSlangExceedsVersion1ByteBound};
		}
		return {};
	}

	auto GenerateMaterialProgramSlang(const MIR::FModule& IR) -> FMaterialSourceGenerationResult
	{
		std::vector<FMaterialCompilerParameterDeclaration> Parameters;
		for (const auto& Node : IR.Nodes)
		{
			if (Node.Opcode != EMaterialProgramOpcode::Parameter
				&& Node.Opcode != EMaterialProgramOpcode::TextureParameter) continue;
			EMaterialParameterType Type = EMaterialParameterType::Texture;
			switch (Node.ResultType)
			{
			case EMaterialProgramValueType::Float: Type = EMaterialParameterType::Scalar; break;
			case EMaterialProgramValueType::Float2: Type = EMaterialParameterType::Vector2; break;
			case EMaterialProgramValueType::Float3: Type = EMaterialParameterType::Vector; break;
			case EMaterialProgramValueType::Float4: Type = EMaterialParameterType::Vector4; break;
			default: break;
			}
			const FMaterialCompilerParameterDeclaration Parameter{Node.GetParameterId(), Type};
			if (!std::ranges::contains(Parameters, Parameter)) Parameters.push_back(Parameter);
		}
		const auto Layout = CompileMaterialLayout(Parameters);
		if (!Layout) return {.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Generation,
			.Error = FMaterialError(Layout.Validation)}}};
		return GenerateMaterialProgramSlang(IR, Layout.Layout);
	}

	auto MIR::Validate(const MIR::FModule& IR, const FMaterialRenderLayout& Layout) -> FMaterialProgramValidationResult
	{
		const auto Valid = ValidateCompiledMaterialLayout(Layout);
		if (!Valid)
		{
			FMaterialProgramValidationResult Result;
			Result.Diagnostics.push_back(MakeDiagnostic(EMaterialProgramDiagnosticCategory::Generation,
				FMaterialError(Valid)));
			return Result;
		}
		std::vector<FMaterialCompilerParameterDeclaration> Parameters;
		for (const auto& Field : Layout.Fields)
		{
			EMaterialParameterType Type;
			switch (Field.Type)
			{
			case EMaterialRenderValueType::Scalar: Type = EMaterialParameterType::Scalar; break;
			case EMaterialRenderValueType::Vector2: Type = EMaterialParameterType::Vector2; break;
			case EMaterialRenderValueType::Vector3: Type = EMaterialParameterType::Vector; break;
			case EMaterialRenderValueType::Vector4: Type = EMaterialParameterType::Vector4; break;
			default: Type = EMaterialParameterType::Texture; break;
			}
			Parameters.push_back({Field.ParameterId, Type});
		}
		return MIR::Validate(IR, Parameters);
	}

	auto MIR::Validate(const MIR::FModule& IR, std::span<const FMaterialCompilerParameterDeclaration> Parameters)
		-> FMaterialProgramValidationResult
	{
		FMaterialProgramValidationResult Result;
		std::set<FGuid> ParameterIds;
		if (Parameters.size() > MaterialMaxParameterDefinitionCount)
		{
			Result.Diagnostics.push_back(MakeDiagnostic(EMaterialProgramDiagnosticCategory::Generation, EMaterialIRError::ParameterDeclarationCountExceedsBound));
			return Result;
		}
		for (const auto& Parameter : Parameters)
			if (!Parameter.Id.IsValid() || !ParameterIds.insert(Parameter.Id).second
				|| (Parameter.Type != EMaterialParameterType::Scalar && Parameter.Type != EMaterialParameterType::Vector2
					&& Parameter.Type != EMaterialParameterType::Vector && Parameter.Type != EMaterialParameterType::Vector4
					&& Parameter.Type != EMaterialParameterType::Texture))
			{
				Result.Diagnostics.push_back(MakeDiagnostic(EMaterialProgramDiagnosticCategory::Generation, EMaterialIRError::ParameterDeclarationInvalidDuplicated));
				return Result;
			}
		FByteBuffer Canonical;
		const auto Error = MIR::EncodeCanonical(IR, Canonical);
		if (!Error)
		{
			Result.Diagnostics.push_back(MakeDiagnostic(EMaterialProgramDiagnosticCategory::Generation, std::move(Error.Error)));
			return Result;
		}
		// Validate the detached IR directly before indexed source generation.
		const auto Fail = [&](FMaterialError Error) {
			Result.Diagnostics.push_back(MakeDiagnostic(EMaterialProgramDiagnosticCategory::Generation, std::move(Error)));
		};
		std::vector<uint32> Depth(IR.Nodes.size(), 1);
		uint32 LinkCount = IR.SurfaceRoot.bAggregate ? 1u : static_cast<uint32>(
			std::ranges::count(IR.SurfaceRoot.Inputs, true, &MIR::FModule::FSurfaceInput::bExpression));
		for (uint32 Index = 0; Index < IR.Nodes.size(); ++Index)
		{
			const auto& Node = IR.Nodes[Index];
			const auto Signature = GetMaterialProgramNodeSignature(Node.Opcode, Node.ResultType);
			if ((Node.Opcode >= EMaterialProgramOpcode::FunctionInput && Node.Opcode <= EMaterialProgramOpcode::AppendVector) || !Signature || Node.Inputs.size() != Signature->InputCount)
			{
				Fail(EMaterialIRError::OpcodeResultTypeInputCountInvalid);
				return Result;
			}
			LinkCount += static_cast<uint32>(Node.Inputs.size());
			if (LinkCount > MaterialFunctionMaxExpandedLinks)
			{
				Fail(EMaterialIRError::InputCountExceedsExpandedGraphBound);
				return Result;
			}
			for (size_t Slot = 0; Slot < Node.Inputs.size(); ++Slot)
			{
				const auto Input = Node.Inputs[Slot];
				if (Input >= Index)
				{
					Fail(EMaterialIRError::InvalidInputOrder);
					return Result;
				}
				const auto Accepted = Signature->Inputs[Slot];
				if (!std::ranges::contains(Accepted, IR.Nodes[Input].ResultType))
				{
					Fail(EMaterialIRError::InputTypeMismatch);
					return Result;
				}
				Depth[Index] = std::max(Depth[Index], Depth[Input] + 1);
			}
			if (Depth[Index] > MaterialProgramMaxDepth)
			{
				Fail(EMaterialIRError::ExpressionDepthExceedsSupportedBound);
				return Result;
			}
			if (Node.Opcode == EMaterialProgramOpcode::Constant)
			{
				const std::array Values{Node.GetLiteral().X, Node.GetLiteral().Y, Node.GetLiteral().Z, Node.GetLiteral().W};
				for (uint32 Component = 0; Component <= static_cast<uint32>(Node.ResultType); ++Component)
					if (!std::isfinite(Values[Component]))
					{
						Fail(EMaterialIRError::NonFiniteConstant);
						return Result;
					}
			}
			if (Node.Opcode == EMaterialProgramOpcode::Parameter || Node.Opcode == EMaterialProgramOpcode::TextureParameter)
			{
				const auto Field = std::ranges::find(Parameters, Node.GetParameterId(), &FMaterialCompilerParameterDeclaration::Id);
				const auto Expected = [&] {
					switch (Node.ResultType)
					{
					case EMaterialProgramValueType::Float: return EMaterialParameterType::Scalar;
					case EMaterialProgramValueType::Float2: return EMaterialParameterType::Vector2;
					case EMaterialProgramValueType::Float3: return EMaterialParameterType::Vector;
					case EMaterialProgramValueType::Float4: return EMaterialParameterType::Vector4;
					default: return EMaterialParameterType::Texture;
					}
				}();
				if (!Node.GetParameterId().IsValid() || Field == Parameters.end()
					|| Field->Type != Expected)
				{
					Fail(EMaterialIRError::ParameterMissingIncompatibleBindingType);
					return Result;
				}
			}
			if (Node.Opcode == EMaterialProgramOpcode::Swizzle)
			{
				const std::array Mask{Node.GetSwizzle().Components[0], Node.GetSwizzle().Components[1], Node.GetSwizzle().Components[2], Node.GetSwizzle().Components[3]};
				if (Node.GetSwizzle().Length == 0 || Node.GetSwizzle().Length > 4
					|| Node.GetSwizzle().Length != static_cast<uint32>(Node.ResultType) + 1)
				{
					Fail(EMaterialIRError::SwizzleWidthMismatch);
					return Result;
				}
				for (uint8 Component = 0; Component < Node.GetSwizzle().Length; ++Component)
					if (Mask[Component] > static_cast<uint32>(IR.Nodes[Node.Inputs[0]].ResultType))
					{
						Fail(EMaterialIRError::SwizzleComponentExceedsInputWidth);
						return Result;
					}
			}
		}
		for (const auto& Input : IR.SurfaceRoot.Inputs)
			if (!IR.SurfaceRoot.bAggregate && !Input.bExpression)
			{
				const std::array Values{Input.Literal.X, Input.Literal.Y, Input.Literal.Z, Input.Literal.W};
				if (!std::ranges::all_of(Values | std::views::take(static_cast<uint32>(Input.Type) + 1),
					[](float Value) { return std::isfinite(Value); }))
				{
					Fail(EMaterialIRError::NonFiniteSurfaceDefault);
					return Result;
				}
			}
		Result.bSucceeded = true;
		return Result;
	}

	auto GenerateMaterialProgramSlang(const MIR::FModule& IR, const FMaterialRenderLayout& Layout)
		-> FMaterialSourceGenerationResult
	{
		FMaterialSourceGenerationResult Result;
		auto Validation = MIR::Validate(IR, Layout);
		if (!Validation) { Result.Diagnostics = std::move(Validation.Diagnostics); return Result; }
		const auto Error = GenerateMaterialProgramSlangImpl(IR, Layout, Result.Source);
		Result.bSucceeded = static_cast<bool>(Error);
		if (!Error)
			Result.Diagnostics.push_back(MakeDiagnostic(EMaterialProgramDiagnosticCategory::Generation, std::move(Error.Error)));
		return Result;
	}

	auto ValidateMaterialCompilerResult(const FMaterialCompilerResult& Result)
		-> FMaterialLayoutValidationResult
	{
		if (!Result.bSucceeded || !Result.Identity.IsValid()
			|| Result.PassContractVersion != CurrentMaterialPassContractVersion)
			return {.Error = EMaterialLayoutError::InvalidIdentity};
		const auto Expected = CompileMaterialLayout(Result.ActiveParameters);
		if (!Expected) return Expected.Validation;
		if (Expected.Layout != Result.Layout) return {.Error = EMaterialLayoutError::InvalidField};
		return ValidateMaterialCompiledStages(Result.CompiledShaders, Result.Layout);
	}

	auto ValidateMaterialCompiledStages(std::span<const FCompiledShader> Stages,
		const FMaterialRenderLayout& Layout, const FMaterialCompilerResourceLimits& Limits)
		-> FMaterialLayoutValidationResult
	{
		const auto Rejected = FMaterialLayoutValidationResult{.Error = EMaterialLayoutError::InvalidReflection};
		const auto Valid = ValidateCompiledMaterialLayout(Layout, Limits);
		if (!Valid) return Valid;
		constexpr std::array<std::string_view, 4> Entries{"FragmentMain", "GeometryFragmentMain", "ShadowFragmentMain", "HitProxyFragmentMain"};
		if (Stages.size() != Entries.size()) return Rejected;
		for (uint32 Index = 0; Index < Stages.size(); ++Index)
		{
			const auto& Stage = Stages[Index];
			if (!Stage.Code || Stage.Code->empty() || Stage.SourceEntryPoint != Entries[Index]
				|| Stage.Frequency != EShaderFrequency::Fragment || !Stage.Reflection.PushConstantRanges.empty()
				|| Stage.Reflection.ResourceBindings.size() > 2 * Layout.ResourceFieldCount + 9)
				return Rejected;
			std::unordered_set<uint64> Seen;
			for (const auto& Binding : Stage.Reflection.ResourceBindings)
			{
				if (Binding.SetIndex > 1 || Binding.ArraySize != 1 || Binding.StageFlags != EShaderStageFlags::Fragment
					|| !Seen.insert((uint64(Binding.SetIndex) << 32) | Binding.BindingIndex).second) return Rejected;
				ERHIBindingType Expected;
				const auto Slot = Binding.BindingIndex;
				const bool bMaterialSet = Slot == 2 || Slot == 27 || Slot >= MaterialTextureBindingBase;
				if (Binding.SetIndex != (bMaterialSet ? 1u : 0u)) return Rejected;
				if (Slot == 0)
				{
					Expected = ERHIBindingType::UniformBuffer;
					if (Binding.Name != "MeshView") return Rejected;
				}
				else if (Slot == 2 || (Index == 0 && Slot == 1))
				{
					Expected = ERHIBindingType::UniformBuffer;
					if (Binding.Name != (Slot == 2 ? "Material" : "Lighting")) return Rejected;
				}
				else if (Index == 3 && Slot == 27)
				{
					Expected = ERHIBindingType::UniformBuffer;
					if (Binding.Name != "HitProxy") return Rejected;
				}
				else if (Index == 0 && (Slot == 19 || Slot == 20 || Slot == 21 || Slot == 25)) Expected = ERHIBindingType::Texture;
				else if (Index == 0 && (Slot == 22 || Slot == 26)) Expected = ERHIBindingType::Sampler;
				else if (Slot >= MaterialTextureBindingBase && Slot - MaterialTextureBindingBase < 2u * Layout.ResourceFieldCount)
					Expected = (Slot - MaterialTextureBindingBase) % 2 == 0 ? ERHIBindingType::Texture : ERHIBindingType::Sampler;
				else return Rejected;
				if (Binding.Type != Expected) return Rejected;
			}
		}
		return {};
	}

	auto PrepareMaterialProgram(const MIR::FCompilerInput& Input) -> FMaterialPreparedProgram
	{
		const auto Begin = std::chrono::steady_clock::now();
		FMaterialPreparedProgram Prepared;
		Prepared.Normalized = MIR::Normalize(Input);
		Prepared.NormalizationMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - Begin).count();
		Prepared.StaticProperties = Input.StaticProperties;
		Prepared.Environment = Input.Environment;
		return Prepared;
	}

	auto CompilePreparedMaterialProgram(const FMaterialPreparedProgram& Input,
		bool bForceRecompile) -> FMaterialCompilerResult
	{
		FMaterialCompilerResult Result;
		Result.CompilerIdentity = Input.Environment.CompilerIdentity;
		Result.Target = Input.Environment.Target;
		Result.PassContractVersion = Input.Environment.PassContractVersion;
		Result.Timings.NormalizationMicroseconds = Input.NormalizationMicroseconds;
		const auto GenerateBegin = std::chrono::steady_clock::now();
		const auto& Normalized = Input.Normalized;
		if (!Normalized)
		{
			Result.Diagnostics = Normalized.Diagnostics;
			return Result;
		}
		Result.Identity = Normalized.Identity;
		Result.IR = Normalized.IR;
		Result.ActiveParameters = Normalized.ActiveParameters;
		Result.Layout = Normalized.Layout;
		Result.Dependencies = Input.Environment.Dependencies;
		auto Generated = GenerateMaterialProgramSlang(Result.IR, Result.Layout);
		if (!Generated)
		{
			Result.Diagnostics = std::move(Generated.Diagnostics);
			return Result;
		}
		Result.GeneratedSource = std::move(Generated.Source);
		const auto CompileBegin = std::chrono::steady_clock::now();
		Result.Timings.GenerationMicroseconds =
			std::chrono::duration_cast<std::chrono::microseconds>(
				CompileBegin - GenerateBegin).count();
		FGeneratedShaderCompileRequest Request;
		Request.VirtualPath = "/Generated/Materials/" + Result.Identity.ToString();
		Request.Source = Result.GeneratedSource;
		Request.EntryPoints = {
			"FragmentMain", "GeometryFragmentMain", "ShadowFragmentMain", "HitProxyFragmentMain"};
		Request.Frequencies.assign(4, EShaderFrequency::Fragment);
		Request.Macros.emplace_back("DURIN_MATERIAL_BLEND_MODE",
			std::to_string(static_cast<uint8>(Input.StaticProperties.BlendMode)));
		Request.Macros.emplace_back("DURIN_MATERIAL_SHADING_MODEL",
			std::to_string(static_cast<uint8>(Input.StaticProperties.ShadingModel)));
		Request.Macros.emplace_back(
			"DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS",
			std::to_string(std::bit_cast<uint32>(
				CanonicalizeMaterialShaderProperties(Input.StaticProperties).OpacityMaskThreshold)));
		Request.AllowedImportVirtualPrefixes = {
			"/Engine/Material/", "/Engine/Lighting/"};
		Request.bForceRecompile = bForceRecompile;
		FShaderCompilerOutput Output = CompileGeneratedShader(Request);
		const auto CompileEnd = std::chrono::steady_clock::now();
		Result.Timings.CompilationMicroseconds =
			std::chrono::duration_cast<std::chrono::microseconds>(
				CompileEnd - CompileBegin).count();
		if (!Output)
		{
			Result.Diagnostics.push_back(MakeDiagnostic(
				EMaterialProgramDiagnosticCategory::Compile,
				FMaterialError::FromExternal(EMaterialCompileError::ShaderCompilerFailed, FormatShaderError(Output.Error))));
			return Result;
		}
		const auto Reflection = ValidateMaterialCompiledStages(Output.CompiledShaders, Result.Layout, Input.Environment.ResourceLimits);
		if (!Reflection)
		{
			Result.Diagnostics.push_back(MakeDiagnostic(
				EMaterialProgramDiagnosticCategory::Reflection,
				FMaterialError(Reflection)));
			return Result;
		}
		Result.CompiledShaders = std::move(Output.CompiledShaders);
		Result.bSucceeded = true;
		return Result;
	}
	auto MIR::Compile(const MIR::FCompilerInput& Input, bool bForceRecompile) -> FMaterialCompilerResult
	{
		return CompilePreparedMaterialProgram(PrepareMaterialProgram(Input), bForceRecompile);
	}
}
