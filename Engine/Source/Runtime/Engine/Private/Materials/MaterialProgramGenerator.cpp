#include "Shader/MaterialShader.h"
#include "MaterialPreparedProgram.h"
#include "Materials/MaterialShaderMap.h"
#include "Shader/ShaderCompilingManager.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Materials/MaterialAttributeDefinitionMap.h"

#include "Materials/MaterialTypes.h"
#include "Materials/MaterialRenderTypes.h"

namespace Durin
{
	namespace
	{
		auto UsesAuthoredSpecular(const MIR::FModule& IR) -> bool
		{
			const auto& Root = IR.GetAttribute(EMaterialProperty::MP_Specular);
			if (!Root.bExpression) return Root.Literal != FMaterialProgramLiteral{.5f};
			return IR.Nodes[Root.ExpressionIndex].Opcode != EMaterialProgramOpcode::Constant
				|| IR.Nodes[Root.ExpressionIndex].GetLiteral() != FMaterialProgramLiteral{.5f};
		}

		auto DeriveMaterialProgramRequirements(const MIR::FModule& IR)
			-> FMaterialProgramRequirements
		{
			FMaterialProgramRequirements Result;
			if (IR.Domain == EMaterialDomain::PostProcess) { Result.bMaterialView = true; Result.bViewport = true; }
			for (const auto& Node : IR.Nodes)
			{
				switch (Node.Opcode)
				{
				case EMaterialProgramOpcode::Time:
					Result.bMaterialView = true; break;
				case EMaterialProgramOpcode::CameraPosition:
				case EMaterialProgramOpcode::CameraVector:
					Result.bMaterialView = true; Result.bCameraPosition = true; break;
				case EMaterialProgramOpcode::ScreenPosition:
				case EMaterialProgramOpcode::ViewSize:
					Result.bMaterialView = true; Result.bViewport = true; break;
				case EMaterialProgramOpcode::ObjectPosition:
					Result.bMaterialPrimitive = true; Result.bBoundsCenter = true; break;
				case EMaterialProgramOpcode::VertexNormal:
					Result.bVertexNormal = true; break;
				case EMaterialProgramOpcode::TransformPosition:
				case EMaterialProgramOpcode::TransformDirection:
				case EMaterialProgramOpcode::TransformNormal:
				{
					const auto Transform = Node.GetTransform();
					if (Transform.Source == EMaterialCoordinateSpace::View
						|| Transform.Destination == EMaterialCoordinateSpace::View)
					{
						Result.bMaterialView = true;
						Result.bViewTransforms = true;
					}
					if (Transform.Source == EMaterialCoordinateSpace::Object
						|| Transform.Destination == EMaterialCoordinateSpace::Object)
					{
						Result.bMaterialPrimitive = true;
						Result.bObjectTransforms = true;
					}
					if (Transform.Source == EMaterialCoordinateSpace::Tangent
						|| Transform.Destination == EMaterialCoordinateSpace::Tangent)
						Result.bTangentFrame = true;
					break;
				}
				default: break;
				}
			}
			return Result;
		}
	}

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
			case EMaterialProgramValueType::TextureCube: return "TextureCube<float4>";
			case EMaterialProgramValueType::MaterialAttributes: return "FMaterialSurface";
			case EMaterialProgramValueType::StaticBool: return {};
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

		auto MakeDiagnostic(std::string Message) -> FMaterialProgramDiagnostic
		{
			return {.Message = std::move(Message)};
		}
	}

    auto ValidateMaterialEvaluationDomain(const MIR::FModule& IR) -> std::expected<void, std::string>
    {
        for (const auto& Node : IR.Nodes)
        {
			if ((Node.Opcode == EMaterialProgramOpcode::SceneTexture || Node.Opcode == EMaterialProgramOpcode::SceneDepth) && IR.Domain != EMaterialDomain::PostProcess)
			return std::unexpected("Scene textures require the PostProcess domain.");
			if (IR.Domain == EMaterialDomain::PostProcess && (Node.Opcode == EMaterialProgramOpcode::CameraVector || Node.Opcode == EMaterialProgramOpcode::WorldPosition
				|| Node.Opcode == EMaterialProgramOpcode::ObjectPosition || Node.Opcode == EMaterialProgramOpcode::VertexNormal
				|| Node.Opcode == EMaterialProgramOpcode::PrimitiveCustomData || Node.Opcode == EMaterialProgramOpcode::InstanceCustomData
				|| Node.Opcode == EMaterialProgramOpcode::VertexInterpolator || Node.Opcode == EMaterialProgramOpcode::UVChannel
				|| ((Node.Opcode == EMaterialProgramOpcode::TransformPosition || Node.Opcode == EMaterialProgramOpcode::TransformDirection || Node.Opcode == EMaterialProgramOpcode::TransformNormal)
					&& (Node.GetTransform().Source == EMaterialCoordinateSpace::Object || Node.GetTransform().Destination == EMaterialCoordinateSpace::Object
						|| Node.GetTransform().Source == EMaterialCoordinateSpace::Tangent || Node.GetTransform().Destination == EMaterialCoordinateSpace::Tangent))))
			return std::unexpected("PostProcess evaluation cannot require mesh or primitive context.");
        }
        return {};
    }
	static auto GenerateMaterialProgramSlangImpl(const MIR::FModule& IR,
		const FMaterialRenderLayout& Layout,
		std::span<const FMaterialParameterCollectionLayout> Collections,
		std::string& OutSource, std::optional<EMaterialEvaluationStage> StageFilter = {}, const FVertexFactoryType* VertexFactory = nullptr) -> std::expected<void, std::string>
	{
		OutSource.clear();
		if (IR.bAggregateAttributes || std::ranges::any_of(IR.Nodes, [](const auto& Node) {
			return Node.Opcode == EMaterialProgramOpcode::MakeMaterialAttributes;
		})) return std::unexpected("Material source generation requires lowered property roots; normalize aggregate attributes first.");
		if (const auto Domain = ValidateMaterialEvaluationDomain(IR); !Domain) return Domain;
		if (IR.Version != MIR::CurrentVersion
			|| IR.Nodes.size() > MaterialFunctionMaxExpandedNodes)
		{
			return std::unexpected("Invalid material IR for Slang generation.");
		}
        std::vector<bool> PixelReachable(IR.Nodes.size());
        std::function<void(uint32)> VisitPixel = [&](uint32 Index) {
            if (PixelReachable[Index]) return;
            PixelReachable[Index] = true;
            if (IR.Nodes[Index].Opcode == EMaterialProgramOpcode::VertexInterpolator) return;
            for (const auto Child : IR.Nodes[Index].Inputs) VisitPixel(Child);
        };
        for (const auto& Root : IR.GetRoots()) if (Root.bExpression && Root.LegalStages == EMaterialEvaluationStage::Pixel) VisitPixel(Root.ExpressionIndex);
        bool PixelNeedsPrimitiveData = false;
        for (uint32 Index = 0; Index < IR.Nodes.size(); ++Index)
        {
            if (!PixelReachable[Index]) continue;
            const auto& Node = IR.Nodes[Index];
            if (Node.Opcode == EMaterialProgramOpcode::ObjectPosition) PixelNeedsPrimitiveData = true;
            if (Node.Opcode == EMaterialProgramOpcode::TransformPosition || Node.Opcode == EMaterialProgramOpcode::TransformDirection || Node.Opcode == EMaterialProgramOpcode::TransformNormal)
            {
                const auto Transform = Node.GetTransform();
                PixelNeedsPrimitiveData |= Transform.Source == EMaterialCoordinateSpace::Object || Transform.Destination == EMaterialCoordinateSpace::Object;
            }
        }
		std::vector<uint32> Interpolators;
		for (uint32 Index = 0; Index < IR.Nodes.size(); ++Index)
			if (IR.Nodes[Index].Opcode == EMaterialProgramOpcode::VertexInterpolator) Interpolators.push_back(Index);
		if (Interpolators.size() > MaterialProgramMaxVertexInterpolators)
			return std::unexpected("Invalid material IR for Slang generation.");
		std::string FactoryImports;
		if (VertexFactory) for (const auto Module : VertexFactory->GetShaderDescription().ShaderModules) FactoryImports += "import " + std::string(Module) + ";\n";
		else if (!StageFilter || *StageFilter == EMaterialEvaluationStage::Vertex) {
			std::set<std::string_view> Modules;
			for (const auto& D : GetMaterialShaderEntries()) if (D.VertexFactory) for (const auto Module : D.VertexFactory()->GetShaderDescription().ShaderModules) Modules.insert(Module);
			for (const auto Module : Modules) FactoryImports += "import " + std::string(Module) + ";\n";
		}
		OutSource = "module DurinGeneratedMaterial;\n" + FactoryImports;
		OutSource += R"(
import Material.SurfaceMaterial;
import Material.MeshUniforms;
import Material.GBufferDecode;
import Material.SingleLayerWaterLighting;
import SceneData;
#include "Material/SurfacePassResources.slang"

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
)";
		for (uint32 Slot = 0; Slot < Interpolators.size(); ++Slot)
			OutSource += std::format("    {} interpolator{} : TEXCOORD{};\n",
				SlangType(IR.Nodes[Interpolators[Slot]].ResultType), Slot, Slot + 7);
		// Custom channels are flat varyings, so grouped fragments never read the
		// representative draw's primitive uniform for another visible instance.
		uint32 CustomLocation = 7 + static_cast<uint32>(Interpolators.size());
		std::string CopyCustomData = "void CopyPrimitiveCustomData(inout VSOutput input, MaterialPrimitiveUniform primitive)\n{\n";
		for (const auto [Opcode, Name, UniformName] : {
			std::tuple{EMaterialProgramOpcode::PrimitiveCustomData, "customPrimitiveData", "CustomPrimitiveData"},
			std::tuple{EMaterialProgramOpcode::InstanceCustomData, "customInstanceData", "CustomInstanceData"}})
		{
			std::array<bool, 8> Slots{};
			for (const auto& Node : IR.Nodes)
				if (Node.Opcode == Opcode)
				{
					const uint32 First = std::get<MIR::FCustomDataRead>(Node.Payload).Index;
					for (uint32 Component = 0; Component <= static_cast<uint32>(Node.ResultType); ++Component)
						Slots[(First + Component) / 4] = true;
				}
			for (uint32 Slot = 0; Slot < Slots.size(); ++Slot)
				if (Slots[Slot])
				{
					OutSource += std::format("    nointerpolation float4 {}{} : TEXCOORD{};\n", Name, Slot, CustomLocation++);
					CopyCustomData += std::format("    input.{}{} = primitive.{}[{}];\n", Name, Slot, UniformName, Slot);
				}
		}
		if (PixelNeedsPrimitiveData) OutSource += std::format("    nointerpolation uint sceneInstanceId : TEXCOORD{};\n", CustomLocation);
		OutSource += "};\n";
		OutSource += CopyCustomData + "}\n";
        const auto MaterialParameters = BuildMaterialBaseParameterSchema(Layout, Collections);
        if (!MaterialParameters) return std::unexpected(MaterialParameters.error());
        auto Declaration = [&](std::string_view Name) {
            const auto Field = std::ranges::find(MaterialParameters->Fields, Name, &FShaderParameterField::ShaderName);
            return Field == MaterialParameters->Fields.end() ? std::string{} : GenerateShaderParameterDeclaration(Field->ShaderType, Field->ShaderName);
        };
		OutSource += "struct MaterialUniform\n{\n    // Reserved header; view/pass controls are independent parameter owners.\n    float4 Reserved;\n";
		for (uint32 Index = 0; Index < Layout.UniformFieldCount; ++Index)
			OutSource += std::format("    float4 Value{};\n", Index);
		OutSource += "};\n";
		OutSource += Declaration("Material");
		OutSource += std::string("#define DURIN_MATERIAL_PARAMETER(Owner, Name, Type, Reflected, Runtime, Optional, Usage) Type Name;\n#include \"Material/ViewParameters.inl\"\n#undef DURIN_MATERIAL_PARAMETER\n");
		for (const auto& Field : Layout.Fields)
		{
			if (Field.Storage != EMaterialRenderFieldStorage::Resource) continue;
			const auto Index = Field.CompactIndex;
			OutSource += Declaration(GetMaterialTextureParameterName(Index));
            OutSource += Declaration(GetMaterialSamplerParameterName(Index));
		}
		for (uint32 Index = 0; Index < Collections.size(); ++Index)
		{
			OutSource += std::format("struct MaterialCollectionUniform{}\n{{\n    float4 Reserved;\n", Index);
			for (uint32 Field = 0; Field < Collections[Index].UniformLayout.UniformFieldCount; ++Field)
				OutSource += std::format("    float4 Value{};\n", Field);
			OutSource += "};\n";
            OutSource += Declaration(GetMaterialCollectionParameterName(Index));
		}

		auto FindField = [&](const FGuid& Id) -> const FMaterialRenderField* {
			const auto It = std::ranges::find(Layout.Fields, Id, &FMaterialRenderField::ParameterId);
			return It == Layout.Fields.end() ? nullptr : &*It;
		};
		auto FindCollectionField = [&](FGuid CollectionId, FGuid ParameterId)
			-> std::pair<uint32, const FMaterialRenderField*> {
			for (uint32 Index = 0; Index < Collections.size(); ++Index)
				if (Collections[Index].CollectionId == CollectionId)
				{
					const auto Field = std::ranges::find(Collections[Index].UniformLayout.Fields,
						ParameterId, &FMaterialRenderField::ParameterId);
					return {Index, Field == Collections[Index].UniformLayout.Fields.end()
						? nullptr : &*Field};
				}
			return {0, nullptr};
		};
		OutSource += R"(
float2 SelectAuthoredUV(VSOutput input, float channel)
{
    uint index = (uint)clamp(floor(channel + 0.5), 0.0, 3.0);
    return index == 1u ? input.uv1 : (index == 2u ? input.uv2 : (index == 3u ? input.uv3 : input.uv0));
}
float3 MaterialSafeNormalize(float3 value)
{
    float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-12 ? value * rsqrt(lengthSquared) : float3(0.0);
}
float3x3 MaterialTangentToWorld(VSOutput input)
{
    float3 normal = MaterialSafeNormalize(input.worldNormal);
    float3 tangent = input.worldTangent.xyz - normal * dot(normal, input.worldTangent.xyz);
    tangent = MaterialSafeNormalize(tangent);
    float3 bitangent = input.worldTangent.w * MaterialSafeNormalize(cross(normal, tangent));
    return float3x3(tangent, bitangent, normal);
}
)";
		if (IR.Domain == EMaterialDomain::PostProcess) OutSource += "#include \"Material/SceneTextures.slang\"\n";
		for (const auto Stage : {EMaterialEvaluationStage::Vertex, EMaterialEvaluationStage::Pixel})
		{
			const bool bVertex = Stage == EMaterialEvaluationStage::Vertex;
			if (IR.Domain == EMaterialDomain::PostProcess && bVertex) continue;
			if (StageFilter && *StageFilter != Stage) continue;
			std::vector<bool> Reachable(IR.Nodes.size());
			std::vector<uint32> EvaluationOrder;
			std::function<void(uint32)> Visit = [&](uint32 Index) {
				if (Reachable[Index]) return;
				Reachable[Index] = true;
				if (!bVertex && IR.Nodes[Index].Opcode == EMaterialProgramOpcode::VertexInterpolator) return;
				for (const auto Child : IR.Nodes[Index].Inputs) Visit(Child);
				if (bVertex) EvaluationOrder.push_back(Index);
			};
			if (bVertex)
			{
				if (IR.GetAttribute(EMaterialProperty::MP_WorldPositionOffset).bExpression) Visit(IR.GetAttribute(EMaterialProperty::MP_WorldPositionOffset).ExpressionIndex);
				for (const auto Index : Interpolators) Visit(IR.Nodes[Index].Inputs[0]);
			}
			else for (const auto& Root : IR.GetRoots()) if (Root.bExpression && Root.LegalStages == EMaterialEvaluationStage::Pixel) Visit(Root.ExpressionIndex);
			auto CustomOutputs = GetMaterialCustomOutputs() | std::views::filter([&](const auto& Family) {
				return std::ranges::any_of(IR.CustomOutputRoots, [&](const auto& Root) {
					return std::ranges::contains(Family.Outputs, Root.AttributeID, &FMaterialCustomOutputSlot::Id);
				});
			});
			const bool EmitCustom = !bVertex && !CustomOutputs.empty();
			if (EmitCustom)
			{
				for (const auto& Family : CustomOutputs)
					OutSource += std::format("#define DURIN_NUM_{} {}\n", Family.FunctionName, Family.Outputs.size());
				OutSource += "struct FMaterialCustomOutputs\n{\n";
				for (const auto& Family : CustomOutputs)
					for (size_t Slot = 0; Slot < Family.Outputs.size(); ++Slot)
					{
						const auto Type = SlangType(Family.Outputs[Slot].Semantics.Type);
						OutSource += std::format("    {0} value_{1}{2};\n    {0} {1}{2}() {{ return value_{1}{2}; }}\n",
							Type, Family.FunctionName, Slot);
					}
				OutSource += "};\n";
			}
			OutSource += bVertex
				? "VSOutput EvaluateGeneratedVertex(VSOutput input, MaterialPrimitiveUniform MaterialPrimitive, out float3 offset)\n{\n"
				: !EmitCustom ? "FMaterialSurface EvaluateGeneratedMaterial(VSOutput input)\n{\n"
				: "FMaterialSurface EvaluateGeneratedMaterial(VSOutput input, out FMaterialCustomOutputs customOutputs)\n{\n";
            if (!bVertex && PixelNeedsPrimitiveData) OutSource += "    MaterialPrimitiveUniform MaterialPrimitive = TranslateMaterialPrimitive(GetMaterialPrimitiveData(input.sceneInstanceId), MaterialView);\n";
			std::vector<std::string> Expressions(IR.Nodes.size());
			uint32 LocalIndex = 0;
			if (!bVertex)
				for (uint32 Index = 0; Index < IR.Nodes.size(); ++Index)
					if (Reachable[Index]) EvaluationOrder.push_back(Index);
			for (const uint32 Index : EvaluationOrder)
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
				case EMaterialProgramOpcode::TextureSampleParameterCube:
				case EMaterialProgramOpcode::TextureSampleParameter2D:
				case EMaterialProgramOpcode::TextureCoordinates:
				case EMaterialProgramOpcode::MakeMaterialAttributes:
				case EMaterialProgramOpcode::BlendMaterialAttributes:
				case EMaterialProgramOpcode::GetMaterialAttributes:
				case EMaterialProgramOpcode::SetMaterialAttributes:
				case EMaterialProgramOpcode::AppendVector:
				case EMaterialProgramOpcode::StaticBool:
				case EMaterialProgramOpcode::StaticSwitch:
				case EMaterialProgramOpcode::QualitySwitch:
				case EMaterialProgramOpcode::FeatureLevelSwitch:
					// Authored operations must be expanded before source generation.
					break;
				case EMaterialProgramOpcode::VertexInterpolator:
				{
					const auto Slot = std::ranges::find(Interpolators, Index) - Interpolators.begin();
					Expression = std::format("input.interpolator{}", Slot);
					break;
				}
				case EMaterialProgramOpcode::WorldPosition: Expression = "input.worldPosition"; break;
				case EMaterialProgramOpcode::PrimitiveCustomData:
				case EMaterialProgramOpcode::InstanceCustomData:
				{
					const uint32 First = std::get<MIR::FCustomDataRead>(Node.Payload).Index;
					const auto Name = Node.Opcode == EMaterialProgramOpcode::PrimitiveCustomData
						? "customPrimitiveData" : "customInstanceData";
					Expression = std::string(SlangType(Node.ResultType)) + "(";
					for (uint32 Component = 0; Component <= static_cast<uint32>(Node.ResultType); ++Component)
					{
						if (Component) Expression += ", ";
						Expression += std::format("input.{}{}[{}]", Name, (First + Component) / 4, (First + Component) % 4);
					}
					Expression += ")";
					break;
				}
				case EMaterialProgramOpcode::Time: Expression = "MaterialView.Parameters.x"; break;
				case EMaterialProgramOpcode::CameraPosition: Expression = "MaterialView.CameraWorldPosition.xyz"; break;
				case EMaterialProgramOpcode::CameraVector: Expression = "MaterialSafeNormalize(MaterialView.CameraWorldPosition.xyz - input.worldPosition)"; break;
				case EMaterialProgramOpcode::ObjectPosition: Expression = "MaterialPrimitive.BoundsCenter.xyz"; break;
				case EMaterialProgramOpcode::VertexNormal: Expression = "MaterialSafeNormalize(input.worldNormal)"; break;
				case EMaterialProgramOpcode::SceneTexture:
				{
					const auto Read = Node.GetSceneTextureRead();
					Expression = Read.SceneTextureId == EMaterialSceneTextureId::PostProcessInput0
						? std::format("{}({})", Read.Filter == EMaterialSceneTextureFilter::Linear ? "ReadMaterialInput0Linear" : "ReadMaterialInput0Point", Input(0))
						: std::format("float4(ReadMaterialDeviceDepth({0}), ReadMaterialDeviceDepth({0}), ReadMaterialDeviceDepth({0}), ReadMaterialDeviceDepth({0}))", Input(0));
					break;
				}
				case EMaterialProgramOpcode::SceneDepth: Expression = std::format("ReadMaterialSceneDepth({})", Input(0)); break;
				case EMaterialProgramOpcode::ScreenPosition: Expression = "(input.pos.xy - MaterialView.Viewport.xy) * MaterialView.ReciprocalViewport.xy"; break;
				case EMaterialProgramOpcode::ViewSize: Expression = "MaterialView.Viewport.zw"; break;
				case EMaterialProgramOpcode::TransformPosition:
				case EMaterialProgramOpcode::TransformDirection:
				case EMaterialProgramOpcode::TransformNormal:
				{
					const auto Transform = Node.GetTransform();
					const auto Value = Input(0);
					auto ToWorldDirection = [&](std::string_view InputValue) -> std::string {
						switch (Transform.Source)
						{
						case EMaterialCoordinateSpace::Object: return std::format("mul(MaterialPrimitive.LocalToWorld, float4({}, 0.0)).xyz", InputValue);
						case EMaterialCoordinateSpace::World: return std::string(InputValue);
						case EMaterialCoordinateSpace::View: return std::format("mul(MaterialView.ViewToWorld, float4({}, 0.0)).xyz", InputValue);
						case EMaterialCoordinateSpace::Tangent: return std::format("mul({}, MaterialTangentToWorld(input))", InputValue);
						default: return {};
						}
					};
					auto FromWorldDirection = [&](std::string_view InputValue) -> std::string {
						switch (Transform.Destination)
						{
						case EMaterialCoordinateSpace::Object: return std::format("mul(MaterialPrimitive.WorldToLocal, float4({}, 0.0)).xyz", InputValue);
						case EMaterialCoordinateSpace::World: return std::string(InputValue);
						case EMaterialCoordinateSpace::View: return std::format("mul(MaterialView.WorldToView, float4({}, 0.0)).xyz", InputValue);
						case EMaterialCoordinateSpace::Tangent: return std::format("mul(MaterialTangentToWorld(input), {})", InputValue);
						default: return {};
						}
					};
					if (Node.Opcode == EMaterialProgramOpcode::TransformPosition)
					{
						std::string World = Transform.Source == EMaterialCoordinateSpace::Object
							? std::format("mul(MaterialPrimitive.LocalToWorld, float4({}, 1.0)).xyz", Value)
							: Transform.Source == EMaterialCoordinateSpace::View
								? std::format("mul(MaterialView.ViewToWorld, float4({}, 1.0)).xyz", Value) : Value;
						Expression = Transform.Destination == EMaterialCoordinateSpace::Object
							? std::format("mul(MaterialPrimitive.WorldToLocal, float4({}, 1.0)).xyz", World)
							: Transform.Destination == EMaterialCoordinateSpace::View
								? std::format("mul(MaterialView.WorldToView, float4({}, 1.0)).xyz", World) : World;
					}
					else if (Node.Opcode == EMaterialProgramOpcode::TransformDirection)
						Expression = FromWorldDirection(ToWorldDirection(Value));
					else
					{
						auto ToWorldNormal = [&](std::string_view InputValue) -> std::string {
							switch (Transform.Source)
							{
							case EMaterialCoordinateSpace::Object: return std::format("mul(MaterialPrimitive.NormalToWorld, float4({}, 0.0)).xyz", InputValue);
							case EMaterialCoordinateSpace::World: return std::string(InputValue);
							case EMaterialCoordinateSpace::View: return std::format("mul(transpose((float3x3)MaterialView.WorldToView), {})", InputValue);
							case EMaterialCoordinateSpace::Tangent: return std::format("mul({}, MaterialTangentToWorld(input))", InputValue);
							default: return {};
							}
						};
						auto FromWorldNormal = [&](std::string_view InputValue) -> std::string {
							switch (Transform.Destination)
							{
							case EMaterialCoordinateSpace::Object: return std::format("mul(transpose((float3x3)MaterialPrimitive.LocalToWorld), {})", InputValue);
							case EMaterialCoordinateSpace::World: return std::string(InputValue);
							case EMaterialCoordinateSpace::View: return std::format("mul(transpose((float3x3)MaterialView.ViewToWorld), {})", InputValue);
							case EMaterialCoordinateSpace::Tangent: return std::format("mul(MaterialTangentToWorld(input), {})", InputValue);
							default: return {};
							}
						};
						Expression = std::format("MaterialSafeNormalize({})", FromWorldNormal(ToWorldNormal(Value)));
					}
					break;
				}
				case EMaterialProgramOpcode::UVChannel: Expression = std::format("SelectAuthoredUV(input, {})", Input(0)); break;
				case EMaterialProgramOpcode::Sine: Expression = std::format("sin({})", Input(0)); break;
				case EMaterialProgramOpcode::Cosine: Expression = std::format("cos({})", Input(0)); break;
				case EMaterialProgramOpcode::Dot: Expression = std::format("dot({}, {})", Input(0), Input(1)); break;
				case EMaterialProgramOpcode::Cross: Expression = std::format("cross({}, {})", Input(0), Input(1)); break;
				case EMaterialProgramOpcode::Length: Expression = std::format("length({})", Input(0)); break;
				case EMaterialProgramOpcode::Distance: Expression = std::format("distance({}, {})", Input(0), Input(1)); break;
				case EMaterialProgramOpcode::Pow: Expression = std::format("pow({}, {})", Input(0), Input(1)); break;
				case EMaterialProgramOpcode::Sqrt: Expression = std::format("sqrt({})", Input(0)); break;
				case EMaterialProgramOpcode::Exp: Expression = std::format("exp({})", Input(0)); break;
				case EMaterialProgramOpcode::Log: Expression = std::format("log({})", Input(0)); break;
				case EMaterialProgramOpcode::Floor: Expression = std::format("floor({})", Input(0)); break;
				case EMaterialProgramOpcode::Ceil: Expression = std::format("ceil({})", Input(0)); break;
				case EMaterialProgramOpcode::Round: Expression = std::format("round({})", Input(0)); break;
				case EMaterialProgramOpcode::Frac: Expression = std::format("frac({})", Input(0)); break;
				case EMaterialProgramOpcode::Fmod: Expression = std::format("fmod({}, {})", Input(0), Input(1)); break;
				case EMaterialProgramOpcode::Step: Expression = std::format("step({}, {})", Input(0), Input(1)); break;
				case EMaterialProgramOpcode::SmoothStep: Expression = std::format("smoothstep({}, {}, {})", Input(0), Input(1), Input(2)); break;
				case EMaterialProgramOpcode::Sign: Expression = std::format("sign({})", Input(0)); break;
				case EMaterialProgramOpcode::Reflect: Expression = std::format("reflect({}, {})", Input(0), Input(1)); break;
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
				case EMaterialProgramOpcode::CollectionParameter:
				{
					const auto Reference = Node.GetCollectionParameter();
					const auto [CollectionIndex, Field] = FindCollectionField(
						Reference.CollectionId, Reference.ParameterId);
					if (Field)
					{
						constexpr std::array<std::string_view, 4> Swizzles{
							".x", ".xy", ".xyz", ""};
						Expression = std::format("MaterialCollection{}.Value{}{}",
							CollectionIndex, Field->CompactIndex,
							Swizzles[static_cast<size_t>(Node.ResultType)]);
					}
					break;
				}
				case EMaterialProgramOpcode::TextureParameter:
				{
					if (const auto* Field = FindField(Node.GetParameterId()))
						Expression = std::format("MaterialTexture{}", Field->CompactIndex);
					break;
				}
				case EMaterialProgramOpcode::TextureSampleCube:
				case EMaterialProgramOpcode::TextureSample2D:
				{
					const auto& TextureNode = IR.Nodes[Node.Inputs[0]];
					if (const auto* Field = FindField(TextureNode.GetParameterId()))
					{
						const auto Mode = Node.GetTextureReadMode();
						const auto Method = Mode == EMaterialTextureReadMode::Level ? "SampleLevel"
							: Mode == EMaterialTextureReadMode::Bias ? "SampleBias"
							: Mode == EMaterialTextureReadMode::Grad ? "SampleGrad" : "Sample";
						const auto Coordinates = Node.Opcode == EMaterialProgramOpcode::TextureSampleCube
							? std::format("((all(isfinite({0})) && dot({0}, {0}) > 0.0) ? {0} : float3(1,0,0))", Input(1)) : Input(1);
						Expression = std::format("{}.{}(MaterialSampler{}, {}", Input(0), Method, Field->CompactIndex, Coordinates);
						for (size_t Slot = 2; Slot < Node.Inputs.size(); ++Slot) Expression += ", " + Input(Slot);
						Expression += ")";
					}
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
				case EMaterialProgramOpcode::Normalize: Expression = Node.SpatialKind == EMaterialSpatialKind::Normal
					? std::format("MaterialSafeNormalize({})", Input(0)) : std::format("normalize({})", Input(0)); break;
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
					return std::unexpected(std::format("Material IR node cannot be generated. [index {}]", Index));
				}
				const auto SymbolIndex = bVertex ? LocalIndex++ : Index;
				Expressions[Index] = std::format("n{}", SymbolIndex);
				if (Node.ResultType != EMaterialProgramValueType::Texture2D && Node.ResultType != EMaterialProgramValueType::TextureCube)
					OutSource += std::format("    {} n{} = {};\n",
						SlangType(Node.ResultType), SymbolIndex, Expression);
				else
					Expressions[Index] = std::move(Expression);
			}
			if (bVertex)
			{
				for (uint32 Slot = 0; Slot < Interpolators.size(); ++Slot)
					OutSource += std::format("    input.interpolator{} = {};\n", Slot,
						Expressions[IR.Nodes[Interpolators[Slot]].Inputs[0]]);
				const auto& Offset = IR.GetAttribute(EMaterialProperty::MP_WorldPositionOffset);
				OutSource += std::format("    offset = {};\n    input.worldPosition += offset;\n    return input;\n}}\n",
					Offset.bExpression ? Expressions[Offset.ExpressionIndex] : LiteralExpression(Offset.Type, Offset.Literal));
			}
			else
			{
				using P = EMaterialProperty;
				const std::array Fields{
					std::pair{P::MP_BaseColor, "baseColor"}, std::pair{P::MP_Normal, "tangentNormal"},
					std::pair{P::MP_Metallic, "metallic"}, std::pair{P::MP_Roughness, "roughness"},
					std::pair{P::MP_AmbientOcclusion, "ambientOcclusion"}, std::pair{P::MP_EmissiveColor, "emissive"},
					std::pair{P::MP_Opacity, "opacity"}, std::pair{P::MP_OpacityMask, "opacityMask"},
					std::pair{P::MP_Specular, "specular"}, std::pair{P::MP_ClearCoat, "clearCoat"},
					std::pair{P::MP_ClearCoatRoughness, "clearCoatRoughness"},
					std::pair{P::MP_Refraction, "refraction"}
				};
				OutSource += "    FMaterialSurface result;\n";
				for (const auto& [Property, Field] : Fields)
				{
					const auto& Root = IR.GetAttribute(Property);
					if (Root.bExpression && Root.ExpressionIndex >= Expressions.size())
						return std::unexpected("Per-property Surface Root expression is out of bounds.");
					const auto Value = Root.bExpression ? Expressions[Root.ExpressionIndex] : LiteralExpression(Root.Type, Root.Literal);
					OutSource += std::format("    result.{} = {};\n", Field, Value);
				}
				for (const auto& Family : CustomOutputs)
					for (size_t Slot = 0; Slot < Family.Outputs.size(); ++Slot)
					{
						const auto& Field = Family.Outputs[Slot];
						const auto It = std::ranges::find(IR.CustomOutputRoots, Field.Id, &MIR::FModule::FAttributeRoot::AttributeID);
						const auto Value = It == IR.CustomOutputRoots.end() ? LiteralExpression(Field.Semantics.Type, Field.DefaultValue)
							: It->bExpression ? Expressions.at(It->ExpressionIndex) : LiteralExpression(It->Type, It->Literal);
						OutSource += std::format("    customOutputs.value_{}{} = {};\n", Family.FunctionName, Slot, Value);
					}
				OutSource += "#if DURIN_MATERIAL_SHADING_MODEL != 2\n    result.clearCoat = 0.0;\n#endif\n";
				OutSource += IR.Domain == EMaterialDomain::PostProcess ? "    return result;\n}\n" : "    return EvaluateMaterialSurface(result);\n}\n";
				if (EmitCustom) OutSource += "FMaterialSurface EvaluateGeneratedMaterial(VSOutput input)\n{\n"
					"    FMaterialCustomOutputs unused;\n    return EvaluateGeneratedMaterial(input, unused);\n}\n";
			}
		} // Stage-specific expression closures.
		if (IR.Domain == EMaterialDomain::PostProcess) OutSource += "#define DURIN_GENERATED_MATERIAL_POSTPROCESS 1\n#include \"Material/PostProcessPass.slang\"\n";
		else OutSource += std::format("#define DURIN_MATERIAL_AUTHORED_SPECULAR {}\n#define DURIN_GENERATED_MATERIAL 1\n#define DURIN_MATERIAL_VERTEX_STAGE {}\n#define DURIN_MATERIAL_PIXEL_STAGE {}\n#define DURIN_MATERIAL_PIXEL_PRIMITIVE_DATA {}\n#include \"Material/SurfacePasses.slang\"\n",
			UsesAuthoredSpecular(IR) ? 1 : 0, StageFilter != EMaterialEvaluationStage::Pixel ? 1 : 0,
			StageFilter != EMaterialEvaluationStage::Vertex ? 1 : 0, PixelNeedsPrimitiveData ? 1 : 0);
		if (IR.Domain == EMaterialDomain::Surface && (!StageFilter || *StageFilter == EMaterialEvaluationStage::Vertex)) {
			OutSource += "#include \"VertexFactory/MaterialVertexEvaluation.slang\"\n";
			std::set<std::string_view> Includes;
			if (VertexFactory) Includes.insert(VertexFactory->GetShaderDescription().MaterialEntryInclude);
			else for (const auto& D : GetMaterialShaderEntries()) if (D.VertexFactory) Includes.insert(D.VertexFactory()->GetShaderDescription().MaterialEntryInclude);
			for (const auto Include : Includes) OutSource += "#include \"" + std::string(Include) + "\"\n";
		}
		if (OutSource.size() > MaterialProgramMaxCanonicalBytes)
		{
			OutSource.clear();
			return std::unexpected("Generated material Slang exceeds the supported byte bound.");
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
		if (!Layout) return {.Diagnostics = {{.Message = std::format("{}{} [index {}]", GetMaterialLayoutErrorText(Layout.Validation.Error), Layout.Validation.ParameterId.IsValid() ? std::format(" [parameter {}]", Layout.Validation.ParameterId.ToString()) : "", Layout.Validation.FieldIndex)}}};
		return GenerateMaterialProgramSlang(IR, Layout.Layout);
	}

	auto MIR::Validate(const MIR::FModule& IR, const FMaterialRenderLayout& Layout) -> FMaterialProgramValidationResult
	{
		const auto Valid = ValidateCompiledMaterialLayout(Layout);
		if (!Valid)
		{
			FMaterialProgramValidationResult Result;
			Result.Diagnostics.push_back(MakeDiagnostic(std::format("{}{} [index {}]", GetMaterialLayoutErrorText(Valid.Error), Valid.ParameterId.IsValid() ? std::format(" [parameter {}]", Valid.ParameterId.ToString()) : "", Valid.FieldIndex)));
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
			Parameters.push_back({Field.ParameterId, Type, Field.Type == EMaterialRenderValueType::TextureCube
				? EMaterialTextureDimension::TextureCube : EMaterialTextureDimension::Texture2D});
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
			Result.Diagnostics.push_back(MakeDiagnostic("IR parameter declaration count exceeds its bound."));
			return Result;
		}
		for (const auto& Parameter : Parameters)
			if (!Parameter.Id.IsValid() || !ParameterIds.insert(Parameter.Id).second
				|| Parameter.TextureDimension > EMaterialTextureDimension::TextureCube
				|| (Parameter.Type != EMaterialParameterType::Texture && Parameter.TextureDimension != EMaterialTextureDimension::Texture2D)
				|| (Parameter.Type != EMaterialParameterType::Scalar && Parameter.Type != EMaterialParameterType::Vector2
					&& Parameter.Type != EMaterialParameterType::Vector && Parameter.Type != EMaterialParameterType::Vector4
					&& Parameter.Type != EMaterialParameterType::Texture))
			{
				Result.Diagnostics.push_back(MakeDiagnostic("IR parameter declaration is invalid or duplicated."));
				return Result;
			}
		FByteBuffer Canonical;
		const auto Error = MIR::EncodeCanonical(IR, Canonical);
		if (!Error)
		{
			Result.Diagnostics.push_back(MakeDiagnostic(Error.error()));
			return Result;
		}
		// Validate the detached IR directly before indexed source generation.
		const auto Fail = [&](std::string Message) {
			Result.Diagnostics.push_back(MakeDiagnostic(std::move(Message)));
		};
		std::vector<uint32> Depth(IR.Nodes.size(), 1);
		uint32 LinkCount = static_cast<uint32>(std::ranges::count(IR.CustomOutputRoots, true, &MIR::FModule::FAttributeRoot::bExpression));
		LinkCount += IR.bAggregateAttributes ? 1u : static_cast<uint32>(
			std::ranges::count(IR.AttributeRoots, true, &MIR::FModule::FAttributeRoot::bExpression));
		for (uint32 Index = 0; Index < IR.Nodes.size(); ++Index)
		{
			const auto& Node = IR.Nodes[Index];
			const auto Signature = GetMaterialProgramNodeSignature(Node.Opcode, Node.ResultType, Node.GetTextureReadMode());
			if (Node.Opcode == EMaterialProgramOpcode::TextureSampleParameterCube || (Node.Opcode >= EMaterialProgramOpcode::FunctionInput && Node.Opcode <= EMaterialProgramOpcode::AppendVector) || !Signature || Node.Inputs.size() != Signature->InputCount)
			{
				Fail("Material IR opcode, result type, or input count is invalid.");
				return Result;
			}
			LinkCount += static_cast<uint32>(Node.Inputs.size());
			if (LinkCount > MaterialFunctionMaxExpandedLinks)
			{
				Fail("Material IR input count exceeds the expanded graph bound.");
				return Result;
			}
			for (size_t Slot = 0; Slot < Node.Inputs.size(); ++Slot)
			{
				const auto Input = Node.Inputs[Slot];
				if (Input >= Index)
				{
					Fail("Material IR inputs must refer to an earlier expression.");
					return Result;
				}
				const auto Accepted = Signature->Inputs[Slot];
				if (!std::ranges::contains(Accepted, IR.Nodes[Input].ResultType))
				{
					Fail("Material IR input type does not match its opcode signature.");
					return Result;
				}
				Depth[Index] = std::max(Depth[Index], Depth[Input] + 1);
			}
			std::vector<FMaterialValueSemantics> InputSemantics;
			InputSemantics.reserve(Node.Inputs.size());
			for (const auto Input : Node.Inputs)
				InputSemantics.push_back(IR.Nodes[Input].GetSemantics());
			if (Node.ScalarBroadcastMask >> Node.Inputs.size())
			{
				Fail("Material IR input stage, spatial kind, or coordinate space is incompatible with its opcode.");
				return Result;
			}
			for (size_t Slot = 0; Slot < InputSemantics.size(); ++Slot)
				if (Node.ScalarBroadcastMask & (1u << Slot))
				{
					const auto& Broadcast = IR.Nodes[Node.Inputs[Slot]];
					if (Broadcast.Opcode < EMaterialProgramOpcode::Splat2
						|| Broadcast.Opcode > EMaterialProgramOpcode::Splat4
						|| Broadcast.Inputs.size() != 1
						|| IR.Nodes[Broadcast.Inputs[0]].ResultType != EMaterialProgramValueType::Float)
					{
						Fail("Material IR input stage, spatial kind, or coordinate space is incompatible with its opcode.");
						return Result;
					}
					InputSemantics[Slot].Type = EMaterialProgramValueType::Float;
				}
			const auto Transform = Node.Opcode == EMaterialProgramOpcode::TransformPosition
				|| Node.Opcode == EMaterialProgramOpcode::TransformDirection
				|| Node.Opcode == EMaterialProgramOpcode::TransformNormal
				? &std::get<FMaterialTransformPayload>(Node.Payload) : nullptr;
			auto ExpectedSemantics = ResolveMaterialProgramNodeSemantics(
				Node.Opcode, Node.ResultType, InputSemantics, Transform, Node.GetTextureReadMode());
			if (Node.Opcode == EMaterialProgramOpcode::Constant
				&& Node.SpatialKind != EMaterialSpatialKind::None)
				ExpectedSemantics = Node.GetSemantics();
			if (ExpectedSemantics && Node.Opcode == EMaterialProgramOpcode::Swizzle)
			{
				const auto& Input = InputSemantics.front();
				const auto Swizzle = Node.GetSwizzle();
				const bool bIdentity3 = Input.Type == EMaterialProgramValueType::Float3
					&& Swizzle.Length == 3 && Swizzle.Components[0] == 0
					&& Swizzle.Components[1] == 1 && Swizzle.Components[2] == 2;
				const bool bIdentity2 = Input.Type == EMaterialProgramValueType::Float2
					&& Swizzle.Length == 2 && Swizzle.Components[0] == 0
					&& Swizzle.Components[1] == 1;
				const bool bSelectXYZ = Input.Type >= EMaterialProgramValueType::Float3
					&& Input.Type <= EMaterialProgramValueType::Float4
					&& Swizzle.Length == 3 && Swizzle.Components[0] == 0
					&& Swizzle.Components[1] == 1 && Swizzle.Components[2] == 2;
				const bool bSelectXY = Input.Type >= EMaterialProgramValueType::Float2
					&& Input.Type <= EMaterialProgramValueType::Float4
					&& Swizzle.Length == 2 && Swizzle.Components[0] == 0
					&& Swizzle.Components[1] == 1;
				if ((bIdentity3 && Input.Kind != EMaterialSpatialKind::ScreenCoordinate)
					|| (bIdentity2 && Input.Kind == EMaterialSpatialKind::ScreenCoordinate))
					ExpectedSemantics = FMaterialValueSemantics{Node.ResultType,
						Input.Stages, Input.Kind, Input.Space};
				if (Node.SpatialKind != EMaterialSpatialKind::None
					|| Node.CoordinateSpace != EMaterialCoordinateSpace::None)
				{
					const bool bIdentityWidth = (bSelectXYZ
						&& Node.SpatialKind != EMaterialSpatialKind::ScreenCoordinate)
						|| (bSelectXY
							&& Node.SpatialKind == EMaterialSpatialKind::ScreenCoordinate);
					if (Input.Kind == EMaterialSpatialKind::None && bIdentityWidth)
						ExpectedSemantics = FMaterialValueSemantics{Node.ResultType,
							Input.Stages, Node.SpatialKind, Node.CoordinateSpace};
				}
			}
			if (!ExpectedSemantics || *ExpectedSemantics != Node.GetSemantics())
			{
				Fail("Material IR input stage, spatial kind, or coordinate space is incompatible with its opcode.");
				return Result;
			}
			if (Depth[Index] > MaterialProgramMaxDepth)
			{
				Fail("Material IR expression depth exceeds the supported bound.");
				return Result;
			}
			if (Node.Opcode == EMaterialProgramOpcode::Constant)
			{
				const std::array Values{Node.GetLiteral().X, Node.GetLiteral().Y, Node.GetLiteral().Z, Node.GetLiteral().W};
				for (uint32 Component = 0; Component <= static_cast<uint32>(Node.ResultType); ++Component)
					if (!std::isfinite(Values[Component]))
					{
						Fail("Material IR constant components must be finite.");
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
					|| Field->Type != Expected || (Expected == EMaterialParameterType::Texture
						&& Field->TextureDimension != (Node.ResultType == EMaterialProgramValueType::TextureCube
							? EMaterialTextureDimension::TextureCube : EMaterialTextureDimension::Texture2D)))
				{
					Fail("Material IR parameter is missing or has an incompatible binding type.");
					return Result;
				}
			}
			if (Node.Opcode == EMaterialProgramOpcode::TextureSampleCube)
			{
				const auto& Direction = IR.Nodes[Node.Inputs[1]];
				const auto V = Direction.GetLiteral();
				if (Direction.Opcode == EMaterialProgramOpcode::Constant && V.X == 0 && V.Y == 0 && V.Z == 0)
				{
					Fail("TextureCube direction must be nonzero.");
					return Result;
				}
			}
			if (Node.Opcode == EMaterialProgramOpcode::Swizzle)
			{
				const std::array Mask{Node.GetSwizzle().Components[0], Node.GetSwizzle().Components[1], Node.GetSwizzle().Components[2], Node.GetSwizzle().Components[3]};
				if (Node.GetSwizzle().Length == 0 || Node.GetSwizzle().Length > 4
					|| Node.GetSwizzle().Length != static_cast<uint32>(Node.ResultType) + 1)
				{
					Fail("Material IR swizzle length does not match its result width.");
					return Result;
				}
				for (uint8 Component = 0; Component < Node.GetSwizzle().Length; ++Component)
					if (Mask[Component] > static_cast<uint32>(IR.Nodes[Node.Inputs[0]].ResultType))
					{
						Fail("Material IR swizzle component exceeds its input width.");
						return Result;
					}
			}
		}
		for (const auto& Input : IR.GetRoots())
		{
			const auto* Definition = FMaterialAttributeDefinitionMap::Find(Input.AttributeID);
			if (Definition && IR.bAggregateAttributes) continue;
			if (Input.bExpression)
			{
				const auto Actual = IR.Nodes[Input.ExpressionIndex].GetSemantics();
				if (Definition && Definition->Property == EMaterialProperty::MP_WorldPositionOffset
					? !IsMaterialWorldPositionOffsetSemantics(Actual)
					: (Actual.Type != Input.Type || Actual.Kind != Input.SpatialKind
					|| Actual.Space != Input.CoordinateSpace
					|| !MaterialStagesContain(Actual.Stages, Input.LegalStages)))
				{
					Fail("Material IR surface root requires a Pixel value with the exact attribute semantics.");
					return Result;
				}
			}
			if (!Input.bExpression)
			{
				const std::array Values{Input.Literal.X, Input.Literal.Y, Input.Literal.Z, Input.Literal.W};
				if (!std::ranges::all_of(Values | std::views::take(static_cast<uint32>(Input.Type) + 1),
					[](float Value) { return std::isfinite(Value); }))
				{
					Fail("Material IR surface defaults must be finite.");
					return Result;
				}
			}
		}
		Result.bSucceeded = true;
		return Result;
	}

	auto GenerateMaterialProgramSlang(const MIR::FModule& IR, const FMaterialRenderLayout& Layout)
		-> FMaterialSourceGenerationResult
	{
		return GenerateMaterialProgramSlang(IR, Layout, {});
	}

	auto GenerateMaterialProgramSlang(const MIR::FModule& IR,
		const FMaterialRenderLayout& Layout,
		std::span<const FMaterialParameterCollectionLayout> Collections)
		-> FMaterialSourceGenerationResult
	{
		FMaterialSourceGenerationResult Result;
		auto Validation = MIR::Validate(IR, Layout);
		if (!Validation) { Result.Diagnostics = std::move(Validation.Diagnostics); return Result; }
		const auto Error = GenerateMaterialProgramSlangImpl(
			IR, Layout, Collections, Result.Source);
		Result.bSucceeded = static_cast<bool>(Error);
		if (!Error)
			Result.Diagnostics.push_back(MakeDiagnostic(Error.error()));
		return Result;
	}

	auto FMaterialProgram::IsValid() const -> bool
	{
		return bool(ValidateMaterialProgram(*this));
	}

	auto ValidateMaterialCompilerResult(const FMaterialCompilerResult& Result)
		-> FMaterialLayoutValidationResult
	{
		if (!Result) return {.Error = EMaterialLayoutError::InvalidIdentity};
		return ValidateMaterialProgram(Result.Resource);
	}

	auto ValidateMaterialProgram(const FMaterialProgram& Result)
		-> FMaterialLayoutValidationResult
	{
		if (!Result.HasShaderMap() || !Result.GetShaderMap().ShaderMapId.IsValid()
			|| Result.GetShaderMap().PassContractVersion != CurrentMaterialPassContractVersion)
			return {.Error = EMaterialLayoutError::InvalidIdentity};
		const auto& Artifact = Result.GetShaderMap();
		const auto OutputContract = ValidateMaterialCompilationOutput(Artifact, Artifact.ShaderMapId, Artifact.Target, Artifact.CompiledShaders);
		if (!OutputContract) return OutputContract;
		const auto Expected = CompileMaterialLayout(Artifact.ActiveParameters);
		if (!Expected) return Expected.Validation;
		if (Expected.Layout != Artifact.Layout) return {.Error = EMaterialLayoutError::InvalidField};
		if (Result.ActiveCollections.size() > MaterialParameterCollectionMaxPerMaterial)
			return {.Error = EMaterialLayoutError::ResourceLimit};
		FGuid PreviousCollection;
		for (const auto& Collection : Result.ActiveCollections)
		{
			if (!Collection.CollectionId.IsValid()
				|| (PreviousCollection.IsValid()
					&& !(PreviousCollection < Collection.CollectionId))
				|| Collection.SchemaVersion != CurrentMaterialParameterCollectionSchemaVersion
				|| !ValidateCompiledMaterialLayout(Collection.UniformLayout)
				|| Collection.DefaultPayload.size()
					!= Collection.UniformLayout.UniformPayloadSize)
				return {.Error = EMaterialLayoutError::InvalidField};
			PreviousCollection = Collection.CollectionId;
		}
		if (Artifact.CollectionLayouts.empty() && Result.ActiveCollections.empty())
		{
			// The output contract already checked reflection against this layout and empty collection set.
			auto OrderedEntries = Artifact.RequiredEntries;
			std::ranges::sort(OrderedEntries);
			return ValidateMaterialStageSequence(Artifact.CompiledShaders, Artifact.Layout, {}, OrderedEntries);
		}
		return ValidateMaterialCompiledStages(
			Artifact.CompiledShaders, Artifact.Layout, {}, Result.ActiveCollections, Artifact.RequiredEntries);
	}

	auto ValidateMaterialStageSequence(std::span<const FShaderCode> Stages,
		const FMaterialRenderLayout& Layout, const FMaterialCompilerResourceLimits& Limits,
		const FMaterialShaderEntrySet& OrderedEntries)
		-> FMaterialLayoutValidationResult
	{
		const auto Rejected = FMaterialLayoutValidationResult{.Error = EMaterialLayoutError::InvalidReflection};
		const auto Valid = ValidateCompiledMaterialLayout(Layout, Limits);
		if (!Valid) return Valid;
		if (OrderedEntries.empty() || Stages.size() != OrderedEntries.size()) return Rejected;
		if (std::ranges::adjacent_find(OrderedEntries) != OrderedEntries.end()) return Rejected;
		for (const auto& Entry : OrderedEntries) if (!FindMaterialShaderEntry(Entry)) return Rejected;
		if (!std::ranges::any_of(OrderedEntries, [](const auto& E) { return FindMaterialShaderEntry(E)->Domain == EMaterialDomain::PostProcess; }) && (!std::ranges::any_of(OrderedEntries, [](auto E) { return FindMaterialShaderEntry(E)->Frequency == EShaderFrequency::Vertex; })
			|| !std::ranges::any_of(OrderedEntries, [](auto E) { return FindMaterialShaderEntry(E)->Frequency == EShaderFrequency::Fragment; }))) return Rejected;
		for (size_t Index = 0; Index < OrderedEntries.size(); ++Index)
		{
			const auto& Entry = *FindMaterialShaderEntry(OrderedEntries[Index]);
			const auto& Stage = Stages[Index];
			if (Stage.Code.IsEmpty() || Stage.SourceEntryPoint != Entry.Name
				|| Stage.Frequency != Entry.Frequency || !Stage.Reflection.PushConstantRanges.empty())
				return {.Error = EMaterialLayoutError::InvalidReflection, .FieldIndex = static_cast<uint32>(Index)};
		}
		return {};
	}

	auto ValidateMaterialCompiledStages(std::span<const FShaderCode> Stages,
		const FMaterialRenderLayout& Layout, const FMaterialCompilerResourceLimits& Limits,
		std::span<const FMaterialParameterCollectionLayout> Collections, const FMaterialShaderEntrySet& RequiredEntries)
		-> FMaterialLayoutValidationResult
	{
		auto Entries = RequiredEntries;
		std::ranges::sort(Entries);
		if (auto Sequence = ValidateMaterialStageSequence(Stages, Layout, Limits, Entries); !Sequence) return Sequence;
		for (size_t Index = 0; Index < Entries.size(); ++Index)
		{
			const auto& Entry = *FindMaterialShaderEntry(Entries[Index]);
			if (!ValidateMaterialShaderBindings(Stages[Index].Reflection, Entry.ShaderType, Entry.Frequency, Layout, Collections))
				return {.Error = EMaterialLayoutError::InvalidReflection, .FieldIndex = static_cast<uint32>(Index)};
		}
		return {};
	}

	auto PrepareMaterialProgram(const MIR::FCompilerInput& Input) -> FMaterialPreparedProgram
	{
		FMaterialPreparedProgram Prepared;
		Prepared.Normalized = MIR::Normalize(Input);
		Prepared.StaticProperties = Input.StaticProperties;
		Prepared.Environment = Input.Environment;
		return Prepared;
	}
	auto BuildMaterialProgramRequirements(const MIR::FModule& IR) -> FMaterialProgramRequirements
	{ return DeriveMaterialProgramRequirements(IR); }

	auto TranslatePreparedMaterialProgram(const FMaterialPreparedProgram& Input)
		-> FMaterialTranslatedProgram
	{
		FMaterialTranslatedProgram Translation;
		Translation.StaticProperties = Input.StaticProperties;
		Translation.ResourceLimits = Input.Environment.ResourceLimits;
		for (const auto& Source : Input.Normalized.Sources) Translation.Sources.push_back({Source});
		auto& Result = Translation;
		Result.CompilerIdentity = Input.Environment.CompilerIdentity;
		Result.Target = Input.Environment.Target;
		Result.PassContractVersion = Input.Environment.PassContractVersion;
		Result.RequiredEntries = SelectMaterialShaderEntries(Input.StaticProperties, Input.Environment.bIncludeEditorPasses, Input.Normalized.IR.Domain, &Input.Environment);
		Result.Quality = Input.Environment.Quality;
		Result.FeatureLevel = Input.Environment.FeatureLevel;
		Result.StaticBools = Input.Environment.StaticBools;
		if (Input.Environment.BuilderVersion != GetMaterialBuildVersion()
			|| Input.Environment.AdapterVersion != GetMaterialCompileAdapterVersion())
		{
			Result.Diagnostics.push_back(MakeDiagnostic("Material compiler environment identity, target, pass contract, or dependency bounds are invalid."));
			return Translation;
		}
		const auto& Normalized = Input.Normalized;
		if (!Normalized)
		{
			Result.Diagnostics = Normalized.Diagnostics;
			Translation.Sources.clear();
			return Translation;
		}
		Result.Identity = Normalized.Identity;
		Result.IR = Normalized.IR;
		Result.ActiveParameters = Normalized.ActiveParameters;
		Result.ActiveCollections = Normalized.ActiveCollections;
		Result.Layout = Normalized.Layout;
		static_cast<FMaterialCompilationOutput&>(Result) = BuildMaterialCompilationOutput(Result.IR, Result.Layout,
			Result.ActiveParameters, Result.ActiveCollections, Input.StaticProperties, Input.Environment, Result.Identity);
		Result.Dependencies = Input.Environment.Dependencies;
		auto Generated = GenerateMaterialProgramSlang(
			Result.IR, Result.Layout, Result.ActiveCollections);
		if (!Generated)
		{
			Result.Diagnostics = std::move(Generated.Diagnostics);
			return Translation;
		}
		Result.GeneratedSource = std::move(Generated.Source);
		Translation.Status = EMaterialProgramStatus::Ok;
		return Translation;
	}

	static auto MakeCommonMaterialShaderRequest(const FMaterialTranslatedProgram& Translation,
		bool bForceRecompile) -> FGeneratedShaderCompileRequest
	{
		FGeneratedShaderCompileRequest Request;
		Request.Target = Translation.Target == "metal-library-1" ? MetalLibraryShaderTarget
			: Translation.Target == "metal-msl-2.0" ? MetalShaderTarget : VulkanShaderTarget;
		Request.AllowedImportVirtualPrefixes = {"/Engine/"};
		Request.bForceRecompile = bForceRecompile;
		Request.MaximumOutputBytes = 8ull * 1024 * 1024;
		const auto ManifestOptions = MakeMaterialShaderCompileOptions(Request.Target, {.OpacityMaskThreshold = 0.5f});
		const auto FactoryRoots = GetMaterialVertexFactorySourceRoots();
		for (const auto& Dependency : Translation.Dependencies)
		{
			const bool IsMaterialEnvironment = Dependency.VirtualPath == "/Engine/MaterialCompilerEnvironment";
			if (!IsMaterialEnvironment && !std::ranges::contains(FactoryRoots, Dependency.VirtualPath)) continue;
			FShaderSourceTreeDescription Tree;
			Tree.VirtualPath = Dependency.VirtualPath;
			Tree.ContentHash = Dependency.ContentHash;
			Tree.EntryPoints = ManifestOptions.EntryPoints;
			Tree.Frequencies = ManifestOptions.Frequencies;
			Tree.Macros = ManifestOptions.Macros;
			Tree.bCompleteClosure = true;
			if (IsMaterialEnvironment) Request.ExpectedSourceTree = std::move(Tree);
			else Request.AdditionalSourceTrees.push_back(std::move(Tree));
		}
		return Request;
	}

	auto MakeMaterialShaderCompileRequest(const FMaterialTranslatedProgram& Translation,
		bool bForceRecompile) -> FGeneratedShaderCompileRequest
	{
		if (!Translation) return {};
		auto Request = MakeCommonMaterialShaderRequest(Translation, bForceRecompile);
		Request.VirtualPath = "/Generated/Materials/" + Translation.Identity.ToString();
		Request.Source = Translation.GeneratedSource;
		Request.Macros = MakeMaterialShaderCompileOptions(Request.Target, CanonicalizeMaterialShaderProperties(Translation.StaticProperties)).Macros;
		auto OrderedEntries = Translation.RequiredEntries;
		std::ranges::sort(OrderedEntries);
		for (const auto& Name : OrderedEntries)
		{
			const auto& Entry = *FindMaterialShaderEntry(Name);
			Request.EntryPoints.emplace_back(Entry.Name);
			Request.Frequencies.push_back(Entry.Frequency);
		}
		return Request;
	}

	auto MakeMaterialShaderStageRequests(const FMaterialTranslatedProgram& Translation,
		bool bForceRecompile) -> std::vector<FGeneratedShaderCompileRequest>
	{
		std::vector<FGeneratedShaderCompileRequest> Requests;
		if (!Translation) return Requests;
		const auto CommonRequest = MakeCommonMaterialShaderRequest(Translation, bForceRecompile);
		FMaterialCompilerEnvironment Environment;
		Environment.Target = Translation.Target;
		Environment.Quality = Translation.Quality;
		Environment.FeatureLevel = Translation.FeatureLevel;
		Environment.bIncludeEditorPasses = Translation.bIncludeEditorPasses;
		for (const auto& Contract : Translation.Entries)
		{
			const auto* Descriptor = FindMaterialShaderEntry(Contract.Shader.ShaderType);
			if (!Descriptor) return {};
			const bool Vertex = Descriptor->Frequency == EShaderFrequency::Vertex;
			auto Projected = Translation.IR;
			const auto Demanded = GetMaterialEntryProperties(Descriptor->Entry, Translation.StaticProperties);
			if (!Vertex)
				for (auto& Root : Projected.AttributeRoots)
				{
					const auto* Definition = FMaterialAttributeDefinitionMap::Find(Root.AttributeID);
					if (!std::ranges::contains(Demanded, Definition->Property)) { Root.bExpression = false; Root.Literal = Definition->DefaultValue; }
				}
			std::erase_if(Projected.CustomOutputRoots, [&](const auto& Root) {
				for (const auto& Family : GetMaterialCustomOutputs())
					if (Family.Entry == Descriptor->Entry && std::ranges::contains(Family.Outputs, Root.AttributeID, &FMaterialCustomOutputSlot::Id))
						return false;
				return true;
			});
			auto Request = CommonRequest;
			if (!GenerateMaterialProgramSlangImpl(Projected, Translation.Layout, Translation.ActiveCollections, Request.Source,
				Vertex ? EMaterialEvaluationStage::Vertex : EMaterialEvaluationStage::Pixel, Descriptor->VertexFactory ? Descriptor->VertexFactory() : nullptr)) return {};
			Request.VirtualPath = "/Generated/MaterialStages/" + FXxHash128::HashBuffer(Request.Source).ToString();
			Request.EntryPoints = {Contract.Shader.EntryPoint}; Request.Frequencies = {Contract.Shader.Frequency};
			Request.Macros = GetMaterialShaderCompilationOptions(Contract.Shader.ShaderType,
				Translation.StaticProperties, Translation.IR.Domain, Environment).Macros;
			Request.Macros.emplace_back("DURIN_PARAMETER_SCHEMA", std::format("\"{}\"", Contract.ParameterSchemaIdentity.ToString()));
			if (!std::ranges::any_of(Request.Macros, [](const auto& M) { return M.Name == "DURIN_STAGE_PARAMETER_SPACES"; }))
				Request.Macros.emplace_back("DURIN_STAGE_PARAMETER_SPACES", "1");
			Requests.push_back(std::move(Request));
		}
		return Requests;
	}


	auto MIR::Compile(const MIR::FCompilerInput& Input, bool bForceRecompile) -> FMaterialCompilerResult
	{
		return MaterialBuildPrivate::CompileMaterialInput(Input, bForceRecompile).Output;
	}
}
