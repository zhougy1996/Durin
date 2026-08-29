#include "Materials/MaterialProgramCompiler.h"

#include "Materials/MaterialTypes.h"

#include <array>
#include <chrono>
#include <format>

namespace Durin
{
	namespace
	{
		constexpr std::array<std::string_view, 8> GRoleNames{
			"BaseColor", "Normal", "Metallic", "Roughness",
			"AmbientOcclusion", "Emissive", "Opacity", "OpacityMask"};

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
			}
			return {};
		}

		auto FloatExpression(float Value) -> std::string
		{
			const uint32 Bits = Value == 0.0f
				? 0u : std::bit_cast<uint32>(Value);
			return std::format("asfloat(0x{:08x}u)", Bits);
		}

		auto FindRole(FGuid ParameterId, bool bTexture) -> int32
		{
			const auto Kind = bTexture
				? MaterialParameters::EMaterialBuiltinParameterKind::Texture
				: MaterialParameters::EMaterialBuiltinParameterKind::Value;
			const auto Role = MaterialParameters::FindBuiltinParameterRole(
				ParameterId, Kind);
			return Role == MaterialParameters::EMaterialBuiltinParameterRole::Count
				? -1 : static_cast<int32>(Role);
		}

		auto ParameterExpression(FGuid Id) -> std::string
		{
			switch (FindRole(Id, false))
			{
			case 0: return "Material.BaseColor.xyz";
			case 1: return "Material.NormalRoughness.xyz";
			case 2: return "Material.EmissiveMetallic.w";
			case 3: return "Material.NormalRoughness.w";
			case 4: return "Material.SurfaceParams.x";
			case 5: return "Material.EmissiveMetallic.xyz";
			case 6: return "Material.BaseColor.w";
			case 7: return "Material.SurfaceParams.y";
			default: return {};
			}
		}

		auto LiteralExpression(const FMaterialIRNode& Node) -> std::string
		{
			std::array Values{Node.Literal.X, Node.Literal.Y,
				Node.Literal.Z, Node.Literal.W};
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

		auto MakeDiagnostic(EMaterialProgramDiagnosticCategory Category,
			std::string Message) -> FMaterialProgramDiagnostic
		{
			if (Message.size() > MaterialProgramMaxDiagnosticMessageBytes)
				Message.resize(MaterialProgramMaxDiagnosticMessageBytes);
			return {.Category = Category, .Message = std::move(Message)};
		}
	}

	auto GenerateMaterialProgramSlang(const FMaterialIR& IR,
		std::string& OutSource, std::string& OutError) -> bool
	{
		OutSource.clear();
		OutError.clear();
		if (IR.Version != CurrentMaterialIRVersion
			|| IR.Nodes.empty()
			|| IR.Nodes.size() > MaterialProgramMaxNodeCount
			|| IR.SurfaceOutputs.size() != 8)
		{
			OutError = "Invalid material IR for Slang generation.";
			return false;
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
#if DURIN_TERRAIN
    float terrainTransition : TEXCOORD7;
#endif
};
struct MaterialUniform
{
    float4 BaseColor;
    float4 EmissiveMetallic;
    float4 NormalRoughness;
    float4 SurfaceParams;
    float4 UVTransforms[8];
    float4 UVChannels0;
    float4 UVChannels1;
    float4 UVRotations0;
    float4 UVRotations1;
};
[[vk::binding(1, 0)]] ConstantBuffer<FForwardLightingUniform> Lighting;
[[vk::binding(2, 0)]] ConstantBuffer<MaterialUniform> Material;
)";
		for (uint32 Role = 0; Role < GRoleNames.size(); ++Role)
			OutSource += std::format(
				"[[vk::binding({}, 0)]] Texture2D<float4> {}Texture;\n",
				Role + 3, GRoleNames[Role]);
		for (uint32 Role = 0; Role < GRoleNames.size(); ++Role)
			OutSource += std::format(
				"[[vk::binding({}, 0)]] SamplerState {}Sampler;\n",
				Role + 11, GRoleNames[Role]);
		OutSource += R"(
[[vk::binding(19, 0)]] TextureCube<float4> EnvironmentIrradiance;
[[vk::binding(20, 0)]] TextureCube<float4> EnvironmentPrefiltered;
[[vk::binding(21, 0)]] Texture2D<float4> EnvironmentBrdfLut;
[[vk::binding(22, 0)]] SamplerState EnvironmentSampler;
[[vk::binding(25, 0)]] Texture2DArray<float> DirectionalShadowTexture;
[[vk::binding(26, 0)]] SamplerComparisonState DirectionalShadowSampler;

float2 GetMaterialUV(VSOutput input, uint role)
{
    float channel = role < 4u ? Material.UVChannels0[role]
        : Material.UVChannels1[role - 4u];
    uint index = min((uint)(channel + 0.5), 3u);
    float2 uv = index == 1u ? input.uv1
        : (index == 2u ? input.uv2 : (index == 3u ? input.uv3 : input.uv0));
    float4 transform = Material.UVTransforms[role];
    float rotation = role < 4u ? Material.UVRotations0[role]
        : Material.UVRotations1[role - 4u];
    float2 scaled = uv * transform.xy;
    if (rotation == 0.0)
        return scaled + transform.zw;
    float sine;
    float cosine;
    sincos(rotation, sine, cosine);
    return float2(cosine * scaled.x - sine * scaled.y,
        sine * scaled.x + cosine * scaled.y) + transform.zw;
}

FMaterialSurface EvaluateGeneratedMaterial(VSOutput input)
{
)";

		std::vector<std::string> Expressions(IR.Nodes.size());
		for (uint32 Index = 0; Index < IR.Nodes.size(); ++Index)
		{
			const FMaterialIRNode& Node = IR.Nodes[Index];
			auto Input = [&](size_t Slot) -> const std::string& {
				return Expressions[Node.Inputs[Slot]];
			};
			std::string Expression;
			switch (Node.Opcode)
			{
			case EMaterialProgramOpcode::Constant:
				Expression = LiteralExpression(Node); break;
			case EMaterialProgramOpcode::Parameter:
				Expression = ParameterExpression(Node.ParameterId); break;
			case EMaterialProgramOpcode::TextureParameter:
			{
				const int32 Role = FindRole(Node.ParameterId, true);
				if (Role >= 0) Expression = std::format("{}Texture", GRoleNames[Role]);
				break;
			}
			case EMaterialProgramOpcode::TextureCoordinate:
			{
				const int32 Role = FindRole(Node.ParameterId, true);
				if (Role >= 0) Expression = std::format("GetMaterialUV(input, {}u)", Role);
				break;
			}
			case EMaterialProgramOpcode::TextureSample2D:
			{
				const auto& TextureNode = IR.Nodes[Node.Inputs[0]];
				const int32 Role = FindRole(TextureNode.ParameterId, true);
				if (Role >= 0) Expression = std::format(
					"{}.Sample({}Sampler, {})", Input(0), GRoleNames[Role], Input(1));
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
				std::array Mask{Node.SwizzleX, Node.SwizzleY,
					Node.SwizzleZ, Node.SwizzleW};
				Expression = Input(0) + ".";
				for (uint8 Slot = 0; Slot < Node.SwizzleLength; ++Slot)
					Expression += Components[Mask[Slot]];
				break;
			}
			case EMaterialProgramOpcode::Splat2:
			case EMaterialProgramOpcode::Splat3:
			case EMaterialProgramOpcode::Splat4:
				Expression = std::format("{}({})", SlangType(Node.ResultType), Input(0)); break;
			case EMaterialProgramOpcode::TruncateToFloat: Expression = Input(0) + ".x"; break;
			case EMaterialProgramOpcode::TruncateToFloat2: Expression = Input(0) + ".xy"; break;
			case EMaterialProgramOpcode::TruncateToFloat3: Expression = Input(0) + ".xyz"; break;
			case EMaterialProgramOpcode::DecodeNormalRG: Expression = std::format("DecodeTextureNormal({})", Input(0)); break;
			case EMaterialProgramOpcode::BlendNormalsRNM: Expression = std::format("BlendSurfaceNormalsRNM({}, {})", Input(0), Input(1)); break;
			}
			if (Expression.empty())
			{
				OutError = std::format("Material IR node {} cannot be generated.", Index);
				OutSource.clear();
				return false;
			}
			Expressions[Index] = std::format("n{}", Index);
			if (Node.ResultType != EMaterialProgramValueType::Texture2D)
				OutSource += std::format("    {} n{} = {};\n",
					SlangType(Node.ResultType), Index, Expression);
			else
				Expressions[Index] = std::move(Expression);
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
    return result;
}}
)", Expressions[IR.SurfaceOutputs[0]], Expressions[IR.SurfaceOutputs[1]],
			Expressions[IR.SurfaceOutputs[2]], Expressions[IR.SurfaceOutputs[3]],
			Expressions[IR.SurfaceOutputs[4]], Expressions[IR.SurfaceOutputs[5]],
			Expressions[IR.SurfaceOutputs[6]], Expressions[IR.SurfaceOutputs[7]]);
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
        result.normalFrame.shadingNormal, Material.SurfaceParams.w > 0.5);
    return result;
}
bool RejectGeneratedTerrainCoverage(VSOutput input)
{
#if DURIN_TERRAIN && DURIN_MATERIAL_BLEND_MODE != 2
    static const float coverageThresholds[16] =
    {
        0.03125, 0.53125, 0.15625, 0.65625,
        0.78125, 0.28125, 0.90625, 0.40625,
        0.21875, 0.71875, 0.09375, 0.59375,
        0.96875, 0.46875, 0.84375, 0.34375
    };
    uint2 coveragePixel = uint2(input.pos.xy) & uint2(3u, 3u);
    return input.terrainTransition
        >= coverageThresholds[coveragePixel.y * 4u + coveragePixel.x];
#else
    return false;
#endif
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
    FMaterialSurface s = EvaluateGeneratedMaterial(input);
    FResolvedGeneratedSurfaceShading shading =
        ResolveGeneratedSurfaceShading(input, s, isFrontFace);
    if (RejectGeneratedTerrainCoverage(input))
        discard;
#if DURIN_MATERIAL_BLEND_MODE == 1
    if (RejectMaterialMask(s.opacityMask, asfloat(uint(
            DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS))))
        discard;
#endif
    if (Material.SurfaceParams.z < 0.5)
        discard;
    GeometryPassFragmentOutput o;
    o.material = float4(s.baseColor, s.metallic);
    o.normals = float4(
        EncodeOctahedralNormal(shading.normalFrame.shadingNormal),
        EncodeOctahedralNormal(shading.normalFrame.geometricNormal));
    o.surface = float4(shading.effectiveRoughness,
        s.ambientOcclusion, s.opacity, 1.0 / 255.0);
    o.emissive = float4(s.emissive, 0.0);
    return o;
}
[shader("fragment")]
void ShadowFragmentMain(VSOutput input)
{
    float mask = Material.SurfaceParams.y * OpacityMaskTexture.Sample(
        OpacityMaskSampler, GetMaterialUV(input, 7u)).r;
    if (mask < asfloat(uint(DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS)))
        discard;
}
[shader("fragment")]
float4 FragmentMain(
    VSOutput input, bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
    FMaterialSurface s = EvaluateGeneratedMaterial(input);
    FResolvedGeneratedSurfaceShading shading;
    if (Material.SurfaceParams.z >= 0.5)
        shading = ResolveGeneratedSurfaceShading(input, s, isFrontFace);
    if (RejectGeneratedTerrainCoverage(input))
        discard;
#if DURIN_MATERIAL_BLEND_MODE == 1
    if (RejectMaterialMask(s.opacityMask, asfloat(uint(
            DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS))))
        discard;
#endif
	if (Material.SurfaceParams.z < 0.5)
		return float4(s.baseColor + s.emissive, s.opacity);
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
        EnvironmentBrdfLut, EnvironmentSampler);
    return ComposeSurfaceLighting(direct, environment, s.emissive, s.opacity);
}
)";
		if (OutSource.size() > MaterialProgramMaxCanonicalBytes)
		{
			OutError = "Generated material Slang exceeds the version-1 byte bound.";
			OutSource.clear();
			return false;
		}
		return true;
	}

	auto ValidateMaterialCompiledStages(
		std::span<const FCompiledShader> Stages,
		std::string& OutError) -> bool
	{
		OutError.clear();
		constexpr std::array<std::string_view, 3> Entries{
			"FragmentMain", "GeometryFragmentMain", "ShadowFragmentMain"};
		constexpr std::array<std::array<uint32, 24>, 1> Forward{{{
			1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
			17, 18, 19, 20, 21, 22, 25, 26}}};
		constexpr std::array<uint32, 17> GBuffer{
			2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18};
		constexpr std::array<uint32, 3> Shadow{2, 10, 18};
		if (Stages.size() != Entries.size())
		{
			OutError = "Material compilation returned an incomplete stage set.";
			return false;
		}
		auto ExpectedType = [](uint32 Binding) {
			if (Binding == 1 || Binding == 2)
				return ERHIBindingType::UniformBuffer;
			if ((Binding >= 11 && Binding <= 18) || Binding == 22
				|| Binding == 26) return ERHIBindingType::Sampler;
			return ERHIBindingType::Texture;
		};
		for (size_t StageIndex = 0; StageIndex < Stages.size(); ++StageIndex)
		{
			const FCompiledShader& Stage = Stages[StageIndex];
			std::span<const uint32> Expected = StageIndex == 0
				? std::span<const uint32>(Forward[0])
				: (StageIndex == 1 ? std::span<const uint32>(GBuffer)
					: std::span<const uint32>(Shadow));
			if (!Stage.Code || Stage.Code->empty()
				|| Stage.SourceEntryPoint != Entries[StageIndex]
				|| Stage.Frequency != EShaderFrequency::Fragment
				|| !Stage.Reflection.PushConstantRanges.empty()
				|| Stage.Reflection.ResourceBindings.size() > Expected.size())
			{
				OutError = std::format(
					"Material stage {} violates the compiled stage contract.",
					Entries[StageIndex]);
				return false;
			}
			std::unordered_set<uint32> SeenBindings;
			for (const FShaderResourceBinding& Binding
				: Stage.Reflection.ResourceBindings)
			{
				if (std::ranges::find(Expected, Binding.BindingIndex)
					== Expected.end()
					|| !SeenBindings.insert(Binding.BindingIndex).second
					|| Binding.SetIndex != 0 || Binding.ArraySize != 1
					|| Binding.Type != ExpectedType(Binding.BindingIndex)
					|| Binding.StageFlags != EShaderStageFlags::Fragment)
				{
					OutError = std::format(
						"Material stage {} has an incompatible binding {}.",
						Entries[StageIndex], Binding.BindingIndex);
					return false;
				}
			}
		}
		return true;
	}

	auto CompileMaterialProgram(const FMaterialCompilerInput& Input,
		bool bForceRecompile) -> FMaterialCompilerResult
	{
		FMaterialCompilerResult Result;
		Result.CompilerIdentity = Input.Environment.CompilerIdentity;
		Result.Target = Input.Environment.Target;
		Result.PassContractVersion = Input.Environment.PassContractVersion;
		const auto NormalizeBegin = std::chrono::steady_clock::now();
		FMaterialNormalizationResult Normalized = NormalizeMaterialProgram(Input);
		const auto GenerateBegin = std::chrono::steady_clock::now();
		Result.Timings.NormalizationMicroseconds =
			std::chrono::duration_cast<std::chrono::microseconds>(
				GenerateBegin - NormalizeBegin).count();
		if (!Normalized)
		{
			Result.Diagnostics = std::move(Normalized.Diagnostics);
			return Result;
		}
		Result.Identity = Normalized.Identity;
		Result.IR = std::move(Normalized.IR);
		Result.Dependencies = Input.Environment.Dependencies;
		std::string GenerationError;
		if (!GenerateMaterialProgramSlang(
			Result.IR, Result.GeneratedSource, GenerationError))
		{
			Result.Diagnostics.push_back(MakeDiagnostic(
				EMaterialProgramDiagnosticCategory::Generation,
				std::move(GenerationError)));
			return Result;
		}
		const auto CompileBegin = std::chrono::steady_clock::now();
		Result.Timings.GenerationMicroseconds =
			std::chrono::duration_cast<std::chrono::microseconds>(
				CompileBegin - GenerateBegin).count();
		FGeneratedShaderCompileRequest Request;
		Request.VirtualPath = "/Generated/Materials/" + Result.Identity.ToString();
		Request.Source = Result.GeneratedSource;
		Request.EntryPoints = {
			"FragmentMain", "GeometryFragmentMain", "ShadowFragmentMain"};
		Request.Frequencies.assign(3, EShaderFrequency::Fragment);
		Request.Macros.emplace_back("DURIN_TERRAIN", "0");
		Request.Macros.emplace_back("DURIN_MATERIAL_BLEND_MODE",
			std::to_string(static_cast<uint8>(Input.StaticProperties.BlendMode)));
		Request.Macros.emplace_back("DURIN_MATERIAL_SHADING_MODEL",
			std::to_string(static_cast<uint8>(Input.StaticProperties.ShadingModel)));
		Request.Macros.emplace_back(
			"DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS",
			std::to_string(std::bit_cast<uint32>(
				Input.StaticProperties.OpacityMaskThreshold)));
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
				std::move(Output.ErrorMessage)));
			return Result;
		}
		std::string ReflectionError;
		if (!ValidateMaterialCompiledStages(Output.CompiledShaders, ReflectionError))
		{
			Result.Diagnostics.push_back(MakeDiagnostic(
				EMaterialProgramDiagnosticCategory::Reflection,
				std::move(ReflectionError)));
			return Result;
		}
		Result.CompiledShaders = std::move(Output.CompiledShaders);
		Result.bSucceeded = true;
		return Result;
	}
}
