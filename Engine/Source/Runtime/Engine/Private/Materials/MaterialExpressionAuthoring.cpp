#include "MaterialExpressionAuthoring.h"
#include "DObject/DObjectGlobals.h"

namespace Durin::Private
{
	namespace
	{
		auto Metadata(const FMaterialParameterDefinition& Value) -> FMaterialParameterMetadata
		{
			return {Value.Id, Value.Name, Value.DisplayName, Value.GroupName, Value.SortOrder, Value.Presentation};
		}
		auto Connection(const FMaterialProgramLink& Link) -> FMaterialExpressionInput
			{ return {Link.SourceNodeId, Link.SourceOutputIndex, Link.SourceOutputId}; }
		auto Components(const FMaterialInputDefault& Default) -> std::vector<float>
		{
			if (Default.Kind == EMaterialInputDefaultKind::None) return {};
			const std::array Values{Default.Literal.X, Default.Literal.Y, Default.Literal.Z, Default.Literal.W};
			return {Values.begin(), Values.begin() + static_cast<size_t>(Default.Type) + 1};
		}
		auto Coordinates(const FMaterialUVSettings& Value) -> FMaterialExpressionUVSettings
		{
			return {{Value.Channel.Kind != EMaterialInputDefaultKind::None, Value.Channel.Literal.X},
				{Value.Scale.Kind != EMaterialInputDefaultKind::None, FVector2(Value.Scale.Literal.X, Value.Scale.Literal.Y)},
				{Value.Offset.Kind != EMaterialInputDefaultKind::None, FVector2(Value.Offset.Literal.X, Value.Offset.Literal.Y)},
				{Value.Rotation.Kind != EMaterialInputDefaultKind::None, Value.Rotation.Literal.X}};
		}
	}

	auto ConstructFunctionExpressions(DObject* Owner, const FMaterialFunctionGraph& Candidate,
		FMaterialExpressionCollection& OutCollection) -> bool
	{
		FMaterialExpressionCollection Collection;
		for (const auto& Node : Candidate.Nodes)
		{
			DMaterialExpression* Expression = nullptr;
			const FName Name(std::string("Expression_") + Node.Id.ToString());
			switch (Node.Opcode)
			{
			case EMaterialProgramOpcode::Constant:
				switch (Node.ResultType)
				{
				case EMaterialProgramValueType::Float:
				{
					auto* Value = NewObject<DMaterialExpressionScalarConstant>(Owner, Name);
					Value->Value = Node.Literal.X; Expression = Value; break;
				}
				case EMaterialProgramValueType::Float2:
				{
					auto* Value = NewObject<DMaterialExpressionVector2Constant>(Owner, Name);
					Value->Value = {Node.Literal.X, Node.Literal.Y}; Expression = Value; break;
				}
				case EMaterialProgramValueType::Float3:
				{
					auto* Value = NewObject<DMaterialExpressionVector3Constant>(Owner, Name);
					Value->Value = {Node.Literal.X, Node.Literal.Y, Node.Literal.Z}; Expression = Value; break;
				}
				case EMaterialProgramValueType::Float4:
				{
					auto* Value = NewObject<DMaterialExpressionVector4Constant>(Owner, Name);
					Value->Value = {Node.Literal.X, Node.Literal.Y, Node.Literal.Z, Node.Literal.W}; Expression = Value; break;
				}
				default: return false;
				}
				break;
			case EMaterialProgramOpcode::Parameter:
			{
				DMaterialExpressionParameter* Parameter = nullptr;
				switch (Node.Parameter.Type)
				{
				case EMaterialParameterType::Scalar:
				{
					auto* Value = NewObject<DMaterialExpressionScalarParameter>(Owner, Name);
					Value->DefaultValue = Node.Parameter.Value.ScalarValue;
					Value->bHasRange = Node.Parameter.bHasRange;
					Value->MinimumValue = Node.Parameter.MinimumValue;
					Value->MaximumValue = Node.Parameter.MaximumValue;
					Parameter = Value; break;
				}
				case EMaterialParameterType::Vector2:
				{
					auto* Value = NewObject<DMaterialExpressionVector2Parameter>(Owner, Name);
					Value->DefaultValue = Node.Parameter.Value.Vector2Value;
					Parameter = Value; break;
				}
				case EMaterialParameterType::Vector:
				{
					auto* Value = NewObject<DMaterialExpressionVector3Parameter>(Owner, Name);
					Value->DefaultValue = Node.Parameter.Value.VectorValue;
					Parameter = Value; break;
				}
				case EMaterialParameterType::Vector4:
				{
					auto* Value = NewObject<DMaterialExpressionVector4Parameter>(Owner, Name);
					Value->DefaultValue = Node.Parameter.Value.Vector4Value;
					Parameter = Value; break;
				}
				default: return false;
				}
				Parameter->Metadata = Metadata(Node.Parameter);
				Expression = Parameter; break;
			}
			case EMaterialProgramOpcode::TextureParameter:
			case EMaterialProgramOpcode::TextureSampleParameter2D:
			{
				DMaterialExpressionTextureParameter* Value;
				if (Node.Opcode == EMaterialProgramOpcode::TextureSampleParameter2D)
				{
					auto* Sample = NewObject<DMaterialExpressionTextureSampleParameter2D>(Owner, Name);
					Sample->UV = Connection(Node.Inputs[0]); Sample->UVSettings = Coordinates(Node.UVSettings);
					Value = Sample;
				}
				else Value = NewObject<DMaterialExpressionTextureParameter>(Owner, Name);
				Value->Metadata = Metadata(Node.Parameter); Value->TextureUsage = Node.Parameter.TextureUsage;
				Value->DefaultValue = {Node.Parameter.Value.TextureValue, Node.Parameter.Value.SamplerState, Node.Parameter.Value.TextureFallback};
				Expression = Value; break;
			}
			case EMaterialProgramOpcode::Add:
			{
				auto* Value = NewObject<DMaterialExpressionAdd>(Owner, Name);
				Value->ResultType = Node.ResultType;
				Value->A = Connection(Node.Inputs[0]);
				Value->ADefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Value->B = Connection(Node.Inputs[1]);
				Value->BDefault = Components(GetMaterialNodeInputDefault(Node, 1));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::Subtract:
			{
				auto* Value = NewObject<DMaterialExpressionSubtract>(Owner, Name);
				Value->ResultType = Node.ResultType;
				Value->A = Connection(Node.Inputs[0]);
				Value->ADefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Value->B = Connection(Node.Inputs[1]);
				Value->BDefault = Components(GetMaterialNodeInputDefault(Node, 1));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::Multiply:
			{
				auto* Value = NewObject<DMaterialExpressionMultiply>(Owner, Name);
				Value->ResultType = Node.ResultType;
				Value->A = Connection(Node.Inputs[0]);
				Value->ADefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Value->B = Connection(Node.Inputs[1]);
				Value->BDefault = Components(GetMaterialNodeInputDefault(Node, 1));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::Divide:
			{
				auto* Value = NewObject<DMaterialExpressionDivide>(Owner, Name);
				Value->ResultType = Node.ResultType;
				Value->A = Connection(Node.Inputs[0]);
				Value->ADefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Value->B = Connection(Node.Inputs[1]);
				Value->BDefault = Components(GetMaterialNodeInputDefault(Node, 1));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::Minimum:
			{
				auto* Value = NewObject<DMaterialExpressionMinimum>(Owner, Name);
				Value->ResultType = Node.ResultType;
				Value->A = Connection(Node.Inputs[0]);
				Value->ADefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Value->B = Connection(Node.Inputs[1]);
				Value->BDefault = Components(GetMaterialNodeInputDefault(Node, 1));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::Maximum:
			{
				auto* Value = NewObject<DMaterialExpressionMaximum>(Owner, Name);
				Value->ResultType = Node.ResultType;
				Value->A = Connection(Node.Inputs[0]);
				Value->ADefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Value->B = Connection(Node.Inputs[1]);
				Value->BDefault = Components(GetMaterialNodeInputDefault(Node, 1));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::Negate:
			{
				auto* Value = NewObject<DMaterialExpressionNegate>(Owner, Name);
				Value->ResultType = Node.ResultType;
				Value->Input = Connection(Node.Inputs[0]);
				Value->InputDefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::OneMinus:
			{
				auto* Value = NewObject<DMaterialExpressionOneMinus>(Owner, Name);
				Value->ResultType = Node.ResultType;
				Value->Input = Connection(Node.Inputs[0]);
				Value->InputDefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::Absolute:
			{
				auto* Value = NewObject<DMaterialExpressionAbsolute>(Owner, Name);
				Value->ResultType = Node.ResultType;
				Value->Input = Connection(Node.Inputs[0]);
				Value->InputDefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::Saturate:
			{
				auto* Value = NewObject<DMaterialExpressionSaturate>(Owner, Name);
				Value->ResultType = Node.ResultType;
				Value->Input = Connection(Node.Inputs[0]);
				Value->InputDefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::Normalize:
			{
				auto* Value = NewObject<DMaterialExpressionNormalize>(Owner, Name);
				Value->ResultType = Node.ResultType;
				Value->Input = Connection(Node.Inputs[0]);
				Value->InputDefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::Sine:
			{
				auto* Value = NewObject<DMaterialExpressionSine>(Owner, Name);
				Value->ResultType = Node.ResultType;
				Value->Input = Connection(Node.Inputs[0]);
				Value->InputDefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::Cosine:
			{
				auto* Value = NewObject<DMaterialExpressionCosine>(Owner, Name);
				Value->ResultType = Node.ResultType;
				Value->Input = Connection(Node.Inputs[0]);
				Value->InputDefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::Clamp:
			{
				auto* Value = NewObject<DMaterialExpressionClamp>(Owner, Name);
				Value->ResultType = Node.ResultType;
				Value->Input = Connection(Node.Inputs[0]);
				Value->InputDefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Value->Minimum = Connection(Node.Inputs[1]);
				Value->MinimumDefault = Components(GetMaterialNodeInputDefault(Node, 1));
				Value->Maximum = Connection(Node.Inputs[2]);
				Value->MaximumDefault = Components(GetMaterialNodeInputDefault(Node, 2));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::Lerp:
			{
				auto* Value = NewObject<DMaterialExpressionLerp>(Owner, Name);
				Value->ResultType = Node.ResultType;
				Value->A = Connection(Node.Inputs[0]);
				Value->ADefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Value->B = Connection(Node.Inputs[1]);
				Value->BDefault = Components(GetMaterialNodeInputDefault(Node, 1));
				Value->Alpha = Connection(Node.Inputs[2]);
				Value->AlphaDefault = Components(GetMaterialNodeInputDefault(Node, 2));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::MakeFloat2:
			{
				auto* Value = NewObject<DMaterialExpressionMakeVector2>(Owner, Name);
				Value->X = Connection(Node.Inputs[0]);
				Value->XDefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Value->Y = Connection(Node.Inputs[1]);
				Value->YDefault = Components(GetMaterialNodeInputDefault(Node, 1));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::MakeFloat3:
			{
				auto* Value = NewObject<DMaterialExpressionMakeVector3>(Owner, Name);
				Value->X = Connection(Node.Inputs[0]);
				Value->XDefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Value->Y = Connection(Node.Inputs[1]);
				Value->YDefault = Components(GetMaterialNodeInputDefault(Node, 1));
				Value->Z = Connection(Node.Inputs[2]);
				Value->ZDefault = Components(GetMaterialNodeInputDefault(Node, 2));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::MakeFloat4:
			{
				auto* Value = NewObject<DMaterialExpressionMakeVector4>(Owner, Name);
				Value->X = Connection(Node.Inputs[0]);
				Value->XDefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Value->Y = Connection(Node.Inputs[1]);
				Value->YDefault = Components(GetMaterialNodeInputDefault(Node, 1));
				Value->Z = Connection(Node.Inputs[2]);
				Value->ZDefault = Components(GetMaterialNodeInputDefault(Node, 2));
				Value->W = Connection(Node.Inputs[3]);
				Value->WDefault = Components(GetMaterialNodeInputDefault(Node, 3));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::Splat2:
			{
				auto* Value = NewObject<DMaterialExpressionSplat2>(Owner, Name);
				Value->Input = Connection(Node.Inputs[0]);
				Value->InputDefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::Splat3:
			{
				auto* Value = NewObject<DMaterialExpressionSplat3>(Owner, Name);
				Value->Input = Connection(Node.Inputs[0]);
				Value->InputDefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::Splat4:
			{
				auto* Value = NewObject<DMaterialExpressionSplat4>(Owner, Name);
				Value->Input = Connection(Node.Inputs[0]);
				Value->InputDefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::TruncateToFloat:
			{
				auto* Value = NewObject<DMaterialExpressionTruncateToScalar>(Owner, Name);
				Value->Input = Connection(Node.Inputs[0]);
				Value->InputDefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::TruncateToFloat2:
			{
				auto* Value = NewObject<DMaterialExpressionTruncateToVector2>(Owner, Name);
				Value->Input = Connection(Node.Inputs[0]);
				Value->InputDefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::TruncateToFloat3:
			{
				auto* Value = NewObject<DMaterialExpressionTruncateToVector3>(Owner, Name);
				Value->Input = Connection(Node.Inputs[0]);
				Value->InputDefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::DecodeNormalRG:
			{
				auto* Value = NewObject<DMaterialExpressionDecodeNormalRG>(Owner, Name);
				Value->Input = Connection(Node.Inputs[0]);
				Value->InputDefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::BlendNormalsRNM:
			{
				auto* Value = NewObject<DMaterialExpressionBlendNormalsRNM>(Owner, Name);
				Value->Base = Connection(Node.Inputs[0]);
				Value->BaseDefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Value->Detail = Connection(Node.Inputs[1]);
				Value->DetailDefault = Components(GetMaterialNodeInputDefault(Node, 1));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::UVChannel:
			{
				auto* Value = NewObject<DMaterialExpressionUVChannel>(Owner, Name);
				Value->Channel = Connection(Node.Inputs[0]);
				Value->ChannelDefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::MakeSurface:
			{
				auto* Value = NewObject<DMaterialExpressionMakeSurface>(Owner, Name);
				Value->BaseColor = Connection(Node.Inputs[0]);
				Value->BaseColorDefault = Components(GetMaterialNodeInputDefault(Node, 0));
				Value->Normal = Connection(Node.Inputs[1]);
				Value->NormalDefault = Components(GetMaterialNodeInputDefault(Node, 1));
				Value->Metallic = Connection(Node.Inputs[2]);
				Value->MetallicDefault = Components(GetMaterialNodeInputDefault(Node, 2));
				Value->Roughness = Connection(Node.Inputs[3]);
				Value->RoughnessDefault = Components(GetMaterialNodeInputDefault(Node, 3));
				Value->AmbientOcclusion = Connection(Node.Inputs[4]);
				Value->AmbientOcclusionDefault = Components(GetMaterialNodeInputDefault(Node, 4));
				Value->Emissive = Connection(Node.Inputs[5]);
				Value->EmissiveDefault = Components(GetMaterialNodeInputDefault(Node, 5));
				Value->Opacity = Connection(Node.Inputs[6]);
				Value->OpacityDefault = Components(GetMaterialNodeInputDefault(Node, 6));
				Value->OpacityMask = Connection(Node.Inputs[7]);
				Value->OpacityMaskDefault = Components(GetMaterialNodeInputDefault(Node, 7));
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::Swizzle:
			{
				auto* Value = NewObject<DMaterialExpressionSwizzle>(Owner, Name);
				Value->Input = Connection(Node.Inputs[0]);
				Value->InputDefault = Components(GetMaterialNodeInputDefault(Node, 0));
				const std::array Mask{Node.SwizzleX, Node.SwizzleY, Node.SwizzleZ, Node.SwizzleW};
				Value->Components.assign(Mask.begin(), Mask.begin() + Node.SwizzleLength);
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::TextureCoordinates:
			{
				auto* Value = NewObject<DMaterialExpressionTextureCoordinates>(Owner, Name);
				Value->Channel = Connection(Node.Inputs[0]);
				Value->Scale = Connection(Node.Inputs[1]);
				Value->Offset = Connection(Node.Inputs[2]);
				Value->Rotation = Connection(Node.Inputs[3]);
				Value->Defaults = Coordinates(Node.UVSettings);
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::TextureSample2D:
			{
				auto* Value = NewObject<DMaterialExpressionTextureSample2D>(Owner, Name);
				Value->Texture = Connection(Node.Inputs[0]);
				Value->UV = Connection(Node.Inputs[1]);
				Value->UVSettings = Coordinates(Node.UVSettings);
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::GetSurfaceAttributes:
			{
				auto* Value = NewObject<DMaterialExpressionGetSurfaceAttributes>(Owner, Name);
				Value->Surface = Connection(Node.Inputs[0]);
				Value->AttributeMask = Node.SurfaceAttributeMask;
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::SetSurfaceAttributes:
			{
				auto* Value = NewObject<DMaterialExpressionSetSurfaceAttributes>(Owner, Name);
				Value->Surface = Connection(Node.Inputs[0]);
				for (const auto& Attribute : Node.SurfaceAttributes) Value->Attributes.push_back({Attribute.Attribute, Connection(Attribute.Source)});
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::FunctionInput:
			{
				auto* Value = NewObject<DMaterialExpressionFunctionInput>(Owner, Name);
				Value->PortId = Node.FunctionPortId;
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::FunctionOutput:
			{
				auto* Value = NewObject<DMaterialExpressionFunctionOutput>(Owner, Name);
				Value->PortId = Node.FunctionPortId;
				Value->Source = Connection(Node.Inputs[0]);
				Expression = Value;
				break;
			}
			case EMaterialProgramOpcode::FunctionCall:
			{
				auto* Value = NewObject<DMaterialExpressionFunctionCall>(Owner, Name);
				const auto Call = std::ranges::find(Candidate.Calls, Node.Id, &FMaterialFunctionCall::NodeId);
				if (Call == Candidate.Calls.end()) return false;
				Value->Function = Call->Function; Value->Outputs = Call->Outputs;
				for (const auto& Input : Call->Inputs) Value->Inputs.push_back({Input.InputId, Input.ExpectedType, Connection(Input.Source), Components(Input.Default)});
				Expression = Value;
				break;
			}
			default: return false;
			}
			Expression->Id = Node.Id;
			Collection.Expressions.emplace_back(Expression);
		}
		OutCollection = std::move(Collection);
		return true;
	}
	auto ConstructMaterialExpressions(DObject* Owner, const FMaterialProgram& Program,
		std::span<const FMaterialFunctionCall> Calls, FMaterialExpressionCollection& OutCollection) -> bool
	{
		return ConstructFunctionExpressions(Owner, {.Nodes = Program.Nodes, .Calls = {Calls.begin(), Calls.end()}}, OutCollection);
	}

	auto ConstructMaterialOutputs(const FMaterialSurfaceOutputs& Source) -> FMaterialExpressionSurfaceOutputs
	{
		FMaterialExpressionSurfaceOutputs Result;
		Result.Surface = {Source.Surface.SourceNodeId, Source.Surface.SourceOutputIndex, Source.Surface.SourceOutputId};
		Result.BaseColor = {Source.BaseColor.SourceNodeId, Source.BaseColor.SourceOutputIndex, Source.BaseColor.SourceOutputId};
		Result.Normal = {Source.Normal.SourceNodeId, Source.Normal.SourceOutputIndex, Source.Normal.SourceOutputId};
		Result.Metallic = {Source.Metallic.SourceNodeId, Source.Metallic.SourceOutputIndex, Source.Metallic.SourceOutputId};
		Result.Roughness = {Source.Roughness.SourceNodeId, Source.Roughness.SourceOutputIndex, Source.Roughness.SourceOutputId};
		Result.AmbientOcclusion = {Source.AmbientOcclusion.SourceNodeId, Source.AmbientOcclusion.SourceOutputIndex, Source.AmbientOcclusion.SourceOutputId};
		Result.Emissive = {Source.Emissive.SourceNodeId, Source.Emissive.SourceOutputIndex, Source.Emissive.SourceOutputId};
		Result.Opacity = {Source.Opacity.SourceNodeId, Source.Opacity.SourceOutputIndex, Source.Opacity.SourceOutputId};
		Result.OpacityMask = {Source.OpacityMask.SourceNodeId, Source.OpacityMask.SourceOutputIndex, Source.OpacityMask.SourceOutputId};
		Result.BaseColorDefault = {Source.BaseColorDefault.X, Source.BaseColorDefault.Y, Source.BaseColorDefault.Z};
		Result.NormalDefault = {Source.NormalDefault.X, Source.NormalDefault.Y, Source.NormalDefault.Z};
		Result.MetallicDefault = Source.MetallicDefault.X;
		Result.RoughnessDefault = Source.RoughnessDefault.X;
		Result.AmbientOcclusionDefault = Source.AmbientOcclusionDefault.X;
		Result.EmissiveDefault = {Source.EmissiveDefault.X, Source.EmissiveDefault.Y, Source.EmissiveDefault.Z};
		Result.OpacityDefault = Source.OpacityDefault.X;
		Result.OpacityMaskDefault = Source.OpacityMaskDefault.X;
		return Result;
	}

	auto ProjectMaterialOutputs(const FMaterialExpressionSurfaceOutputs& Source) -> FMaterialSurfaceOutputs
	{
		FMaterialSurfaceOutputs Result;
		Result.Surface = {Source.Surface.ExpressionId, Source.Surface.OutputIndex, Source.Surface.OutputId};
		Result.BaseColor = {Source.BaseColor.ExpressionId, Source.BaseColor.OutputIndex, Source.BaseColor.OutputId};
		Result.Normal = {Source.Normal.ExpressionId, Source.Normal.OutputIndex, Source.Normal.OutputId};
		Result.Metallic = {Source.Metallic.ExpressionId, Source.Metallic.OutputIndex, Source.Metallic.OutputId};
		Result.Roughness = {Source.Roughness.ExpressionId, Source.Roughness.OutputIndex, Source.Roughness.OutputId};
		Result.AmbientOcclusion = {Source.AmbientOcclusion.ExpressionId, Source.AmbientOcclusion.OutputIndex, Source.AmbientOcclusion.OutputId};
		Result.Emissive = {Source.Emissive.ExpressionId, Source.Emissive.OutputIndex, Source.Emissive.OutputId};
		Result.Opacity = {Source.Opacity.ExpressionId, Source.Opacity.OutputIndex, Source.Opacity.OutputId};
		Result.OpacityMask = {Source.OpacityMask.ExpressionId, Source.OpacityMask.OutputIndex, Source.OpacityMask.OutputId};
		Result.BaseColorDefault = {static_cast<float>(Source.BaseColorDefault.x), static_cast<float>(Source.BaseColorDefault.y), static_cast<float>(Source.BaseColorDefault.z)};
		Result.NormalDefault = {static_cast<float>(Source.NormalDefault.x), static_cast<float>(Source.NormalDefault.y), static_cast<float>(Source.NormalDefault.z)};
		Result.MetallicDefault = {Source.MetallicDefault};
		Result.RoughnessDefault = {Source.RoughnessDefault};
		Result.AmbientOcclusionDefault = {Source.AmbientOcclusionDefault};
		Result.EmissiveDefault = {static_cast<float>(Source.EmissiveDefault.x), static_cast<float>(Source.EmissiveDefault.y), static_cast<float>(Source.EmissiveDefault.z)};
		Result.OpacityDefault = {Source.OpacityDefault};
		Result.OpacityMaskDefault = {Source.OpacityMaskDefault};
		return Result;
	}

}
