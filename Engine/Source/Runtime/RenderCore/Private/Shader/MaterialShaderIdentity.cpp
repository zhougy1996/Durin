#include "Shader/MaterialShaderIdentity.h"

#include <tuple>

namespace Durin
{
	namespace
	{
		auto UpdateMaterialIdentity(
			FXxHash128Builder& Builder,
			const FMaterialShaderPermutationIdentity& Identity) -> void
		{
			Builder.UpdateValue(Identity.Material.RenderLayout.Version);
			Builder.UpdateValue(Identity.Material.RenderLayout.Id.A);
			Builder.UpdateValue(Identity.Material.RenderLayout.Id.B);
			Builder.UpdateValue(Identity.Material.RenderLayout.Id.C);
			Builder.UpdateValue(Identity.Material.RenderLayout.Id.D);
			Builder.UpdateValue(Identity.Material.ProgramIdentity.Digest.HashLow);
			Builder.UpdateValue(Identity.Material.ProgramIdentity.Digest.HashHigh);
			Builder.UpdateValue(Identity.Material.BlendMode.Value);
			Builder.UpdateValue(Identity.Material.ShadingModel.Value);
			Builder.UpdateValue(std::bit_cast<uint32>(
				Identity.Material.OpacityMaskThreshold));
			Builder.Update(Identity.ShaderType);
			Builder.UpdateValue(uint8{0});
			Builder.Update(Identity.EntryPoint);
			Builder.UpdateValue(uint8{0});
			Builder.Update(Identity.Target);
			Builder.UpdateValue(uint8{0});
			Builder.UpdateValue(Identity.PermutationId);
			Builder.UpdateValue(static_cast<uint8>(Identity.Frequency));
		}

		auto MakeOrderingTuple(
			const FMaterialShaderPermutationIdentity& Identity)
		{
			return std::tie(
				Identity.Material.RenderLayout.Version,
				Identity.Material.RenderLayout.Id,
				Identity.Material.ProgramIdentity.Digest.HashHigh,
				Identity.Material.ProgramIdentity.Digest.HashLow,
				Identity.Material.BlendMode.Value,
				Identity.Material.ShadingModel.Value,
				Identity.Material.OpacityMaskThreshold,
				Identity.ShaderType,
				Identity.EntryPoint,
				Identity.Target,
				Identity.PermutationId,
				Identity.Frequency);
		}
	}

	auto GetMaterialShaderIdentityHash(
		const FMaterialShaderPermutationIdentity& Identity) -> FXxHash128
	{
		FXxHash128Builder Builder;
		UpdateMaterialIdentity(Builder, Identity);
		return Builder.Finalize();
	}

	auto GetMaterialShaderIdentityHash(
		const FMeshMaterialShaderPermutationIdentity& Identity) -> FXxHash128
	{
		FXxHash128Builder Builder;
		UpdateMaterialIdentity(Builder, Identity.Material);
		Builder.Update(Identity.VertexFactoryType);
		Builder.UpdateValue(uint8{0});
		Builder.UpdateValue(Identity.MeshPassKey);
		Builder.UpdateValue(Identity.MeshPermutationId);
		return Builder.Finalize();
	}

	auto GetMaterialShaderIdentityText(
		const FMaterialShaderPermutationIdentity& Identity) -> std::string
	{
		return std::format(
			"layout-version={},layout-id={},program={},blend={},shading={},mask-bits={},type={},entry={},target={},permutation={},frequency={}",
			Identity.Material.RenderLayout.Version,
			Identity.Material.RenderLayout.Id.ToString(),
			Identity.Material.ProgramIdentity.ToString(),
			Identity.Material.BlendMode.Value,
			Identity.Material.ShadingModel.Value,
			std::bit_cast<uint32>(Identity.Material.OpacityMaskThreshold),
			Identity.ShaderType, Identity.EntryPoint, Identity.Target,
			Identity.PermutationId, static_cast<uint8>(Identity.Frequency));
	}

	auto GetMaterialShaderIdentityText(
		const FMeshMaterialShaderPermutationIdentity& Identity) -> std::string
	{
		return std::format(
			"{},vertex-factory={},mesh-pass={},mesh-permutation={}",
			GetMaterialShaderIdentityText(Identity.Material),
			Identity.VertexFactoryType, Identity.MeshPassKey,
			Identity.MeshPermutationId);
	}

	auto MaterialShaderIdentityLess(
		const FMaterialShaderPermutationIdentity& Left,
		const FMaterialShaderPermutationIdentity& Right) -> bool
	{
		return MakeOrderingTuple(Left) < MakeOrderingTuple(Right);
	}

	auto MaterialShaderIdentityLess(
		const FMeshMaterialShaderPermutationIdentity& Left,
		const FMeshMaterialShaderPermutationIdentity& Right) -> bool
	{
		if (MaterialShaderIdentityLess(Left.Material, Right.Material)) return true;
		if (MaterialShaderIdentityLess(Right.Material, Left.Material)) return false;
		return std::tie(Left.VertexFactoryType, Left.MeshPassKey,
			Left.MeshPermutationId)
			< std::tie(Right.VertexFactoryType, Right.MeshPassKey,
				Right.MeshPermutationId);
	}
}
