#include "SkeletalMesh/SkeletalAssetBuild.h"

#include "SkeletalMesh/SkeletalBuild.h"

namespace Durin
{
	auto PrepareSkeletalMeshPayload(DSkeletalMesh& Mesh, std::string& OutError) -> bool
	{
		DSkeleton* Skeleton = Mesh.GetSkeleton();
		if (!Skeleton)
		{
			OutError = "SkeletalMesh canonical build requires a Skeleton.";
			return false;
		}
		const FSkeletalPayloadSerializationContext Context{
			.SkeletonBoneCount = Skeleton->GetBoneCount(),
			.MaterialSlotCount = Mesh.GetNumMaterialSlots(),
			.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
			.TargetProfile = ESkeletalPayloadTargetProfile::Game};
		FSkeletalMeshDerivedDataResult Result;
		if (!BuildSkeletalMeshDerivedData({
			.ImportedDataIdentity = Mesh.GetImportedData().GetIdentity(),
			.SkeletonCompatibilityIdentity = Mesh.GetSkeletonCompatibilityIdentity(),
			.Context = Context,
			.LoadPayload = [Imported = Mesh.GetImportedData(), Context](std::string& Error)
				-> std::shared_ptr<const FSkeletalMeshPayloadData> {
				FSkeletalMeshPayloadData Payload = Imported.Decode(
					Context.SkeletonBoneCount, Context.MaterialSlotCount, Error);
				return Error.empty()
					? std::make_shared<const FSkeletalMeshPayloadData>(std::move(Payload)) : nullptr;
			}}, Result, OutError)) return false;
		return Mesh.SetAssetData({
			.Skeleton = Skeleton,
			.ValidationSkeleton = Skeleton,
			.SkeletonCompatibilityIdentity = Mesh.GetSkeletonCompatibilityIdentity(),
			.MeshNodeBindTransform = Mesh.GetMeshNodeBindTransform(),
			.MaterialSlots = {Mesh.GetMaterialSlots().begin(), Mesh.GetMaterialSlots().end()},
			.Payload = std::move(Result.Payload),
			.ImportedData = Mesh.GetImportedData()}, OutError);
	}

	auto PrepareAnimationClipPayload(DAnimationClip& Clip, std::string& OutError) -> bool
	{
		DSkeleton* Skeleton = Clip.GetSkeleton();
		if (!Skeleton)
		{
			OutError = "AnimationClip canonical build requires a Skeleton.";
			return false;
		}
		const FSkeletalPayloadSerializationContext Context{
			.SkeletonBoneCount = Skeleton->GetBoneCount(),
			.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
			.TargetProfile = ESkeletalPayloadTargetProfile::Game};
		FAnimationClipDerivedDataResult Result;
		if (!BuildAnimationClipDerivedData({
			.ImportedDataIdentity = Clip.GetImportedData().GetIdentity(),
			.SkeletonCompatibilityIdentity = Clip.GetSkeletonCompatibilityIdentity(),
			.Context = Context,
			.LoadPayload = [Imported = Clip.GetImportedData(), Context](std::string& Error)
				-> std::shared_ptr<const FAnimationClipPayloadData> {
				FAnimationClipPayloadData Payload = Imported.Decode(Context.SkeletonBoneCount, Error);
				return Error.empty()
					? std::make_shared<const FAnimationClipPayloadData>(std::move(Payload)) : nullptr;
			}}, Result, OutError)) return false;
		return Clip.SetAssetData({
			.Skeleton = Skeleton,
			.ValidationSkeleton = Skeleton,
			.SkeletonCompatibilityIdentity = Clip.GetSkeletonCompatibilityIdentity(),
			.ClipName = Clip.GetClipName(),
			.Payload = std::move(Result.Payload),
			.ImportedData = Clip.GetImportedData()}, OutError);
	}
}
