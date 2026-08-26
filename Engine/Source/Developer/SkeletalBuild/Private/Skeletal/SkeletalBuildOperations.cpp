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
					Key, Output.StoreDiagnostic)};
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
					Key, Output.StoreDiagnostic)};
		OutError.clear();
		return true;
	}

	auto LoadSkeletalMeshDerivedData(
		std::string_view Key,
		const FSkeletalPayloadSerializationContext& Context,
		FSkeletalMeshPayloadData& OutPayload,
		std::string& OutMessage) -> bool
	{
		FBuildOutput Output;
		std::string Error;
		const bool bLoaded = ExecuteSkeletalSession(Private::SkeletalMeshFunctionIdentity,
			Private::SkeletalMeshInputName, Key, {}, {}, Context, {}, false,
			Output, OutPayload, Error);
		OutMessage = bLoaded ? Output.Diagnostic : std::move(Error);
		return bLoaded;
	}

	auto LoadAnimationClipDerivedData(
		std::string_view Key,
		const FSkeletalPayloadSerializationContext& Context,
		FAnimationClipPayloadData& OutPayload,
		std::string& OutMessage) -> bool
	{
		FBuildOutput Output;
		std::string Error;
		const bool bLoaded = ExecuteSkeletalSession(Private::AnimationClipFunctionIdentity,
			Private::AnimationClipInputName, Key, {}, {}, Context, {}, false,
			Output, OutPayload, Error);
		OutMessage = bLoaded ? Output.Diagnostic : std::move(Error);
		return bLoaded;
	}

}
