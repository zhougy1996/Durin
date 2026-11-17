#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialProgramCompiler.h"

namespace Durin::MaterialBuildPrivate
{
	template<class T> auto TransferMaterialEntrySet(T& Transfer, FMaterialShaderEntrySet& Entries) -> void
	{
		Transfer.Sequence(Entries, MaterialProgramMaxShaderEntries, [&](auto& Entry) {
			Transfer.String(Entry, 128);
			Transfer.Check(FindMaterialShaderEntry(Entry) != nullptr);
		});
	}
	template<class T> auto TransferMaterialCompilationEntries(T& Transfer, FMaterialCompilationOutput& Output) -> void
	{
		Transfer.Value(Output.BindingSchemaVersion);
		Transfer.Check(Output.BindingSchemaVersion == MaterialBindingSchemaVersion);
		Transfer.Value(Output.Domain); Transfer.Value(Output.Quality); Transfer.Value(Output.FeatureLevel); Transfer.Value(Output.bIncludeEditorPasses);
		Transfer.Sequence(Output.Entries, MaterialProgramMaxShaderEntries, [&](auto& Entry) {
			auto& Key = Entry.Shader;
			Transfer.Value(Key.Material.RenderLayout.Version); Transfer.Value(Key.Material.RenderLayout.Id);
			Transfer.Value(Key.Material.ProgramIdentity.Digest);
			Transfer.Value(Key.Material.BlendMode.Value); Transfer.Value(Key.Material.ShadingModel.Value);
			Transfer.Value(Key.Material.OpacityMaskThreshold);
			Transfer.String(Key.ShaderType, 128); Transfer.String(Key.EntryPoint, 128); Transfer.String(Key.Target, 128);
			Transfer.Value(Key.PermutationId); Transfer.Value(Key.Frequency); Transfer.Value(Key.PassContractVersion);
			Transfer.Value(Key.OutputLayout.Version); Transfer.Value(Key.OutputLayout.Id);
			bool Mesh = Entry.Mesh.has_value(); Transfer.Value(Mesh);
			if (Mesh)
			{
				if (!Entry.Mesh) Entry.Mesh.emplace();
				Transfer.Check(Transfer.IsLoading() || Entry.Mesh->Material == Key);
				Entry.Mesh->Material = Key;
				Transfer.String(Entry.Mesh->VertexFactoryType, 128);
				Transfer.Value(Entry.Mesh->MeshPassKey); Transfer.Value(Entry.Mesh->MeshPermutationId);
			}
			else Entry.Mesh.reset();
            Transfer.Sequence(Entry.Bindings, 256, [&](auto& Binding) {
                Transfer.String(Binding.Name, 256); Transfer.String(Binding.QualifiedName, 1024);
                Transfer.String(Binding.OwnerName, 128); Transfer.Value(Binding.ResourceIndex);
                Transfer.Value(Binding.SetIndex); Transfer.Value(Binding.BindingIndex);
                Transfer.Value(Binding.Type); Transfer.Value(Binding.RuntimeType); Transfer.Value(Binding.ArraySize);
                Transfer.Value(Binding.Scope);
                Transfer.Value(Binding.ResourceShape); Transfer.Value(Binding.ConstantBufferSize);
                Transfer.Check(!Binding.Name.empty() && !Binding.QualifiedName.empty() && !Binding.OwnerName.empty());
                Transfer.Check(Binding.Type <= ERHIBindingType::StorageImage && Binding.RuntimeType <= ERHIBindingType::StorageImage
                    && Binding.Scope <= EShaderParameterBindingScope::Pass && Binding.ArraySize > 0 && Binding.ArraySize <= 1024);
                Transfer.Check(Binding.ResourceShape <= EShaderResourceShape::Texture1DArray && Binding.ConstantBufferSize <= 16 * 1024 * 1024);
            });
            Transfer.Value(Entry.ParameterSchemaIdentity); Transfer.Value(Entry.ParameterMapIdentity);
			auto& Requirements = Entry.Requirements;
			Transfer.Sequence(Requirements.AttributeIDs, 64, [&](auto& V) { Transfer.Value(V); });
			Transfer.Sequence(Requirements.ParameterIDs, MaterialRenderMaxFieldCount, [&](auto& V) { Transfer.Value(V); });
			Transfer.Sequence(Requirements.ResourceIndices, MaterialRenderMaxResourceCount, [&](auto& V) { Transfer.Value(V); });
			Transfer.Sequence(Requirements.CollectionIDs, MaterialParameterCollectionMaxPerMaterial, [&](auto& V) { Transfer.Value(V); });
			Transfer.Sequence(Requirements.SceneTextures, 8, [&](auto& V) { Transfer.Value(V.InputId); Transfer.Value(V.Filter); Transfer.Value(V.bRequired); });
			Transfer.Sequence(Requirements.Interpolators, MaterialProgramMaxVertexInterpolators, [&](auto& V) {
				Transfer.Value(V.Type); Transfer.Value(V.Stages); Transfer.Value(V.Kind); Transfer.Value(V.Space);
			});
			Transfer.Value(Requirements.bUsesWPO); Transfer.Value(Requirements.bUsesMask);
			auto& Q = Requirements.Context;
			Transfer.Value(Q.bMaterialView); Transfer.Value(Q.bMaterialPrimitive); Transfer.Value(Q.bCameraPosition);
			Transfer.Value(Q.bViewport); Transfer.Value(Q.bViewTransforms); Transfer.Value(Q.bObjectTransforms);
			Transfer.Value(Q.bBoundsCenter); Transfer.Value(Q.bTangentFrame); Transfer.Value(Q.bVertexNormal);
		});
	}
}
