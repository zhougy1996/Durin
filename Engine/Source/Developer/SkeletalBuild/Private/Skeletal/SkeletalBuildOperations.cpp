#include "Skeletal/SkeletalBuildOperations.h"

#include "DerivedDataCache/DerivedDataBuildSession.h"
#include "SkeletalBuildFunctionRegistry.h"
#include "Skeletal/SkeletalBuildFunctions.h"

namespace Durin::Asset
{
	using namespace ::Durin::DerivedData;

	namespace
	{
		template<typename T>
		auto DecodeSelectedPayload(const FBuildValue& Value, std::string_view Key,
			const FSkeletalPayloadSerializationContext& Context,
			T& OutPayload, std::string& OutError) -> bool
		{
			if constexpr (std::same_as<T, FSkeletalMeshPayloadData>)
				return Private::DecodeSkeletalMeshPayload(
					Value, Key, Context, OutPayload, OutError);
			else
				return Private::DecodeAnimationClipPayload(
					Value, Key, Context, OutPayload, OutError);
		}

		template<typename T>
		auto ExecuteSkeletalSession(const FBuildFunctionIdentity& Identity,
			std::string_view InputName, std::string_view Key,
			std::span<const std::byte> KeyBytes, std::span<const std::byte> LocalBytes,
			const FSkeletalPayloadSerializationContext& Context,
			std::string_view SkeletonIdentity, bool bRequireStore,
			FBuildOutput& OutOutput, T& OutPayload, std::string& OutError) -> bool
		{
			if (!EnsureSkeletalBuildFunctions(&OutError)) return false;
			FBuildDefinition Definition;
			FBuildDefinitionBuilder Builder(Identity, std::string(Private::SkeletalValueName));
			Builder.SetKey(FBuildKey::FromString(Key), KeyBytes)
				.AddTargetFact("Platform", "Win64").AddTargetFact("Profile", "Game")
				.AddTargetFact("SkeletonBoneCount", std::to_string(Context.SkeletonBoneCount))
				.AddTargetFact("MaterialSlotCount", std::to_string(Context.MaterialSlotCount));
			if (!SkeletonIdentity.empty()) Builder.AddTargetFact("SkeletonIdentity", std::string(SkeletonIdentity));
			if (!LocalBytes.empty())
			{
				Builder.AddTargetFact("PayloadFingerprint", FXxHash128::HashBuffer(LocalBytes).ToString())
					.AddInput(FBuildValue::FromOwned(std::string(InputName),
						std::vector<std::byte>(LocalBytes.begin(), LocalBytes.end())));
			}
			if (!Builder.Build(Definition, &OutError)) return false;
			OutOutput = FBuildSession().Build(Definition, {.bQueryCache = true,
				.bAllowLocalBuild = !LocalBytes.empty(), .bStoreBuildResult = !LocalBytes.empty(),
				.bRequireStoreSuccess = bRequireStore, .bReturnData = true});
			if (!OutOutput.Succeeded()) { OutError = OutOutput.Diagnostic; return false; }
			return DecodeSelectedPayload(OutOutput.Value, Key, Context, OutPayload, OutError);
		}
	}

	auto BuildSkeletalMeshProduct(
		FSkeletalMeshBuildRequest Request,
		FSkeletalMeshBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		OutProduct = {};
		if (!Request.Payload || Request.SkeletonBoneCount == 0
			|| Request.SkeletonCompatibilityIdentity
				!= Request.KeyInput.SkeletonCompatibilityIdentity)
		{
			OutError = "SkeletalMesh Build request has incomplete relationship state.";
			return false;
		}
		const FSkeletalPayloadSerializationContext Context{
			.SkeletonBoneCount = Request.SkeletonBoneCount,
			.MaterialSlotCount = Request.MaterialSlotCount,
			.TargetPlatform = Request.KeyInput.TargetPlatform,
			.TargetProfile = Request.KeyInput.TargetProfile};
		std::vector<std::byte> Bytes;
		FSkeletalMeshPayloadData& Payload =
			const_cast<FSkeletalMeshPayloadData&>(*Request.Payload);
		if (!Private::EncodeSkeletalMeshPayload(Payload, Context, Bytes, OutError)) return false;
		Request.KeyInput.PayloadInputFingerprint = FXxHash128::HashBuffer(Bytes);
		const std::string Key = BuildSkeletalMeshDerivedDataKey(Request.KeyInput, OutError);
		if (Key.empty()) return false;
		const std::vector<std::byte> KeyBytes = BuildSkeletalMeshDerivedDataKeyBytes(Request.KeyInput, OutError);
		FBuildOutput Output;
		FSkeletalMeshPayloadData SelectedPayload;
		if (!ExecuteSkeletalSession(Private::SkeletalMeshFunctionIdentity, Private::SkeletalMeshInputName,
			Key, KeyBytes, Bytes, Context, Request.SkeletonCompatibilityIdentity,
			false, Output, SelectedPayload, OutError)) return false;
		OutProduct = {
			.MeshNodeBindTransform = Request.MeshNodeBindTransform,
			.Payload = std::make_shared<const FSkeletalMeshPayloadData>(std::move(SelectedPayload)),
			.SkeletonCompatibilityIdentity = std::move(Request.SkeletonCompatibilityIdentity),
			.DerivedDataKey = Key,
			.Diagnostic = Output.StoreDiagnostic.empty()
				? std::format("{} SkeletalMesh DDC key {}.",
					Output.Status == EBuildStatus::CacheHit ? "Loaded" : "Stored", Key)
				: std::format("SkeletalMesh DDC write failed for key {}: {}",
					Key, Output.StoreDiagnostic),
			.bLoadedFromDerivedDataCache = Output.Status == EBuildStatus::CacheHit};
		OutError.clear();
		return true;
	}

	auto BuildAnimationClipProduct(
		FAnimationClipBuildRequest Request,
		FAnimationClipBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		OutProduct = {};
		if (!Request.Payload || Request.ClipName.IsNone() || Request.SkeletonBoneCount == 0
			|| Request.SkeletonCompatibilityIdentity
				!= Request.KeyInput.SkeletonCompatibilityIdentity)
		{
			OutError = "AnimationClip Build request has incomplete relationship state.";
			return false;
		}
		const FSkeletalPayloadSerializationContext Context{
			.SkeletonBoneCount = Request.SkeletonBoneCount,
			.TargetPlatform = Request.KeyInput.TargetPlatform,
			.TargetProfile = Request.KeyInput.TargetProfile};
		std::vector<std::byte> Bytes;
		FAnimationClipPayloadData& Payload =
			const_cast<FAnimationClipPayloadData&>(*Request.Payload);
		if (!Private::EncodeAnimationClipPayload(Payload, Context, Bytes, OutError)) return false;
		Request.KeyInput.PayloadInputFingerprint = FXxHash128::HashBuffer(Bytes);
		const std::string Key = BuildAnimationClipDerivedDataKey(Request.KeyInput, OutError);
		if (Key.empty()) return false;
		const std::vector<std::byte> KeyBytes = BuildAnimationClipDerivedDataKeyBytes(Request.KeyInput, OutError);
		FBuildOutput Output;
		FAnimationClipPayloadData SelectedPayload;
		if (!ExecuteSkeletalSession(Private::AnimationClipFunctionIdentity, Private::AnimationClipInputName,
			Key, KeyBytes, Bytes, Context, Request.SkeletonCompatibilityIdentity,
			false, Output, SelectedPayload, OutError)) return false;
		OutProduct = {
			.ClipName = Request.ClipName,
			.Payload = std::make_shared<const FAnimationClipPayloadData>(std::move(SelectedPayload)),
			.SkeletonCompatibilityIdentity = std::move(Request.SkeletonCompatibilityIdentity),
			.DerivedDataKey = Key,
			.Diagnostic = Output.StoreDiagnostic.empty()
				? std::format("{} AnimationClip DDC key {}.",
					Output.Status == EBuildStatus::CacheHit ? "Loaded" : "Stored", Key)
				: std::format("AnimationClip DDC write failed for key {}: {}",
					Key, Output.StoreDiagnostic),
			.bLoadedFromDerivedDataCache = Output.Status == EBuildStatus::CacheHit};
		OutError.clear();
		return true;
	}

	auto RebuildSkeletalMeshFromImportedData(
		DSkeletalMesh& Mesh,
		std::string& OutError) -> bool
	{
		DSkeleton* Skeleton = Mesh.GetSkeleton();
		if (!Skeleton)
		{
			OutError = "SkeletalMesh canonical build requires a Skeleton.";
			return false;
		}
		FSkeletalMeshPayloadData Payload = Mesh.GetImportedData().Decode(
			Skeleton->GetBoneCount(), Mesh.GetNumMaterialSlots(), OutError);
		if (!OutError.empty()) return false;
		FSkeletalMeshBuildKeyInput Key;
		static_cast<FSkeletalBuildKeyFields&>(Key) = {
			.ProviderIdentity = "CanonicalSkeletalMesh",
			.ProviderVersion = SkeletalMeshImportedDataSchemaVersion,
			.ImportedDataIdentity = Mesh.GetImportedData().GetIdentity(),
			.StableOutputIdentity = Mesh.GetObjectPath(),
			.SkeletonCompatibilityIdentity = Mesh.GetSkeletonCompatibilityIdentity(),
			.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
			.TargetProfile = ESkeletalPayloadTargetProfile::Game};
		FSkeletalMeshBuildProduct Product;
		if (!BuildSkeletalMeshProduct({
			.SkeletonBoneCount = Skeleton->GetBoneCount(),
			.SkeletonCompatibilityIdentity = Mesh.GetSkeletonCompatibilityIdentity(),
			.MeshNodeBindTransform = Mesh.GetMeshNodeBindTransform(),
			.MaterialSlotCount = Mesh.GetNumMaterialSlots(),
			.Payload = std::make_shared<const FSkeletalMeshPayloadData>(std::move(Payload)),
			.KeyInput = std::move(Key)}, Product, OutError)) return false;
		return Mesh.PublishBuiltProduct({
			.Skeleton = Skeleton,
			.ValidationSkeleton = Skeleton,
			.SkeletonCompatibilityIdentity = Product.SkeletonCompatibilityIdentity,
			.MeshNodeBindTransform = Product.MeshNodeBindTransform,
			.MaterialSlots = {Mesh.GetMaterialSlots().begin(), Mesh.GetMaterialSlots().end()},
			.Payload = std::move(Product.Payload),
			.CookedPayload = Mesh.GetCookedPayloadDescriptor(),
			.DerivedDataKey = std::move(Product.DerivedDataKey),
			.DiagnosticMessage = std::move(Product.Diagnostic),
			.bLoadedFromDerivedDataCache = Product.bLoadedFromDerivedDataCache,
			.bReplaceImportedData = false,
			.bMarkPackageDirty = false}, OutError);
	}

	auto RebuildAnimationClipFromImportedData(
		DAnimationClip& Clip,
		std::string& OutError) -> bool
	{
		DSkeleton* Skeleton = Clip.GetSkeleton();
		if (!Skeleton)
		{
			OutError = "AnimationClip canonical build requires a Skeleton.";
			return false;
		}
		FAnimationClipPayloadData Payload = Clip.GetImportedData().Decode(
			Skeleton->GetBoneCount(), OutError);
		if (!OutError.empty()) return false;
		FAnimationClipBuildKeyInput Key;
		static_cast<FSkeletalBuildKeyFields&>(Key) = {
			.ProviderIdentity = "CanonicalAnimationClip",
			.ProviderVersion = AnimationClipImportedDataSchemaVersion,
			.ImportedDataIdentity = Clip.GetImportedData().GetIdentity(),
			.StableOutputIdentity = Clip.GetObjectPath(),
			.SkeletonCompatibilityIdentity = Clip.GetSkeletonCompatibilityIdentity(),
			.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
			.TargetProfile = ESkeletalPayloadTargetProfile::Game};
		FAnimationClipBuildProduct Product;
		if (!BuildAnimationClipProduct({
			.SkeletonBoneCount = Skeleton->GetBoneCount(),
			.SkeletonCompatibilityIdentity = Clip.GetSkeletonCompatibilityIdentity(),
			.ClipName = Clip.GetClipName(),
			.Payload = std::make_shared<const FAnimationClipPayloadData>(std::move(Payload)),
			.KeyInput = std::move(Key)}, Product, OutError)) return false;
		return Clip.PublishBuiltProduct({
			.Skeleton = Skeleton,
			.ValidationSkeleton = Skeleton,
			.SkeletonCompatibilityIdentity = Product.SkeletonCompatibilityIdentity,
			.ClipName = Product.ClipName,
			.Payload = std::move(Product.Payload),
			.CookedPayload = Clip.GetCookedPayloadDescriptor(),
			.DerivedDataKey = std::move(Product.DerivedDataKey),
			.DiagnosticMessage = std::move(Product.Diagnostic),
			.bLoadedFromDerivedDataCache = Product.bLoadedFromDerivedDataCache,
			.bReplaceImportedData = false,
			.bMarkPackageDirty = false}, OutError);
	}

}
