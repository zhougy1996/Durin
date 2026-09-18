#include "Materials/MaterialProgramTypes.h"

#include <unordered_set>

namespace Durin
{
	auto IsMaterialSurfaceOutputActive(EMaterialSurfaceOutput Output,
		const FMaterialStaticProperties& Properties) -> bool
	{
		switch (Output)
		{
		case EMaterialSurfaceOutput::Normal:
		case EMaterialSurfaceOutput::Metallic:
		case EMaterialSurfaceOutput::Roughness:
		case EMaterialSurfaceOutput::AmbientOcclusion: return Properties.ShadingModel == EMaterialShadingModel::Lit;
		case EMaterialSurfaceOutput::Opacity: return Properties.BlendMode == EMaterialBlendMode::Translucent;
		case EMaterialSurfaceOutput::OpacityMask: return Properties.BlendMode == EMaterialBlendMode::Masked;
		default: return true;
		}
	}


	auto GetMaterialSurfaceOutputType(EMaterialSurfaceOutput Output)
		-> EMaterialProgramValueType
	{
		switch (Output)
		{
		case EMaterialSurfaceOutput::BaseColor:
		case EMaterialSurfaceOutput::Normal:
		case EMaterialSurfaceOutput::Emissive:
			return EMaterialProgramValueType::Float3;
		case EMaterialSurfaceOutput::Metallic:
		case EMaterialSurfaceOutput::Roughness:
		case EMaterialSurfaceOutput::AmbientOcclusion:
		case EMaterialSurfaceOutput::Opacity:
		case EMaterialSurfaceOutput::OpacityMask:
			return EMaterialProgramValueType::Float;
		}
		return static_cast<EMaterialProgramValueType>(0xff);
	}

	auto GetMaterialSurfaceOutputLink(
		FMaterialSurfaceOutputs& Outputs, EMaterialSurfaceOutput Output)
		-> FMaterialProgramLink&
	{
		return const_cast<FMaterialProgramLink&>(GetMaterialSurfaceOutputLink(
			std::as_const(Outputs), Output));
	}

	auto GetMaterialSurfaceOutputLink(
		const FMaterialSurfaceOutputs& Outputs, EMaterialSurfaceOutput Output)
		-> const FMaterialProgramLink&
	{
		switch (Output)
		{
		case EMaterialSurfaceOutput::BaseColor: return Outputs.BaseColor;
		case EMaterialSurfaceOutput::Normal: return Outputs.Normal;
		case EMaterialSurfaceOutput::Metallic: return Outputs.Metallic;
		case EMaterialSurfaceOutput::Roughness: return Outputs.Roughness;
		case EMaterialSurfaceOutput::AmbientOcclusion: return Outputs.AmbientOcclusion;
		case EMaterialSurfaceOutput::Emissive: return Outputs.Emissive;
		case EMaterialSurfaceOutput::Opacity: return Outputs.Opacity;
		case EMaterialSurfaceOutput::OpacityMask: return Outputs.OpacityMask;
		}
		return Outputs.BaseColor;
	}

	auto GetMaterialSurfaceOutputDefault(
		FMaterialSurfaceOutputs& Outputs, EMaterialSurfaceOutput Output)
		-> FMaterialProgramLiteral&
	{
		return const_cast<FMaterialProgramLiteral&>(GetMaterialSurfaceOutputDefault(
			std::as_const(Outputs), Output));
	}

	auto GetMaterialSurfaceOutputDefault(
		const FMaterialSurfaceOutputs& Outputs, EMaterialSurfaceOutput Output)
		-> const FMaterialProgramLiteral&
	{
		switch (Output)
		{
		case EMaterialSurfaceOutput::BaseColor: return Outputs.BaseColorDefault;
		case EMaterialSurfaceOutput::Normal: return Outputs.NormalDefault;
		case EMaterialSurfaceOutput::Metallic: return Outputs.MetallicDefault;
		case EMaterialSurfaceOutput::Roughness: return Outputs.RoughnessDefault;
		case EMaterialSurfaceOutput::AmbientOcclusion: return Outputs.AmbientOcclusionDefault;
		case EMaterialSurfaceOutput::Emissive: return Outputs.EmissiveDefault;
		case EMaterialSurfaceOutput::Opacity: return Outputs.OpacityDefault;
		case EMaterialSurfaceOutput::OpacityMask: return Outputs.OpacityMaskDefault;
		}
		return Outputs.BaseColorDefault;
	}

	auto SanitizeMaterialGraphPresentation(
		const FMaterialGraphPresentation& Presentation,
		std::span<const FGuid> ExpressionIds) -> FMaterialGraphPresentation
	{
		FMaterialGraphPresentation Result;
		Result.Nodes.reserve(std::min<size_t>(
			Presentation.Nodes.size(), MaterialProgramMaxNodeCount));
		std::unordered_set<FGuid> LiveNodes;
		LiveNodes.reserve(ExpressionIds.size());
		for (const FGuid& Id : ExpressionIds)
			if (Id.IsValid()) LiveNodes.insert(Id);
		std::unordered_set<FGuid> AddedNodes;
		AddedNodes.reserve(Result.Nodes.capacity());
		for (const FMaterialGraphNodePresentation& Node : Presentation.Nodes)
		{
			if (Result.Nodes.size() >= MaterialProgramMaxNodeCount) break;
			if (!Node.NodeId.IsValid() || !LiveNodes.contains(Node.NodeId)
				|| !AddedNodes.insert(Node.NodeId).second
				|| Node.X < -MaterialGraphPresentationCoordinateLimit
				|| Node.X > MaterialGraphPresentationCoordinateLimit
				|| Node.Y < -MaterialGraphPresentationCoordinateLimit
				|| Node.Y > MaterialGraphPresentationCoordinateLimit)
				continue;
			Result.Nodes.push_back(Node);
		}
		std::ranges::sort(Result.Nodes, {},
			&FMaterialGraphNodePresentation::NodeId);
		return Result;
	}

}
