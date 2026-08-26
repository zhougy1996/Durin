#include "Skeletal/SkeletalBuildFunctions.h"

#include "Serialization/Archive.h"

namespace Durin::Asset::Private
{
	const FBuildFunctionIdentity SkeletalMeshFunctionIdentity{
		"Durin.GeometryBuild.SkeletalMesh", 1};
	const FBuildFunctionIdentity AnimationClipFunctionIdentity{
		"Durin.GeometryBuild.AnimationClip", 1};

	namespace
	{
		constexpr uint64 SkeletalDerivedDataBudgetBytes =
			32ull * 1024ull * 1024ull * 1024ull;
		constexpr uint32 SkeletalDerivedDataCleanupDeleteLimit = 256;

		template<typename T>
		auto EncodePayload(T& Payload,
			const FSkeletalPayloadSerializationContext& Context,
			std::vector<std::byte>& OutBytes,
			std::string& OutError) -> bool
		{
			OutBytes.clear();
			FCanonicalMemoryWriter Ar(OutBytes, EArchivePurpose::DerivedDataPayload);
			Payload.Serialize(Ar, Context);
			if (!Ar.HasError())
			{
				OutError.clear();
				return true;
			}
			OutError = Ar.GetFailure()->Message;
			OutBytes.clear();
			return false;
		}

		template<typename T>
		auto DecodePayload(const FBuildValue& Value, std::string_view Key,
			const FSkeletalPayloadSerializationContext& Context,
			T& OutPayload, std::string& OutError) -> bool
		{
			if (Value.GetName() != SkeletalValueName)
			{
				OutError = std::format(
					"Skeletal value name for key {} is incompatible.", Key);
				return false;
			}
			T Candidate;
			FCanonicalMemoryReader Ar(Value.GetBytes(), EArchivePurpose::DerivedDataPayload);
			Candidate.Serialize(Ar, Context);
			if (Ar.HasError() || !RequireArchiveEnd(Ar))
			{
				OutError = Ar.GetFailure()
					? Ar.GetFailure()->Message : "Skeletal payload has trailing bytes.";
				return false;
			}
			OutPayload = std::move(Candidate);
			OutError.clear();
			return true;
		}

		auto ContextFromDefinition(const FBuildDefinition& Definition,
			FSkeletalPayloadSerializationContext& OutContext,
			std::string& OutError) -> bool
		{
			const auto Bones = Definition.GetTargetFact("SkeletonBoneCount");
			const auto Materials = Definition.GetTargetFact("MaterialSlotCount");
			const auto Platform = Definition.GetTargetFact("Platform");
			const auto Profile = Definition.GetTargetFact("Profile");
			if (!Bones || !Materials
				|| Platform != std::optional<std::string_view>("Win64")
				|| Profile != std::optional<std::string_view>("Game"))
			{
				OutError = "Skeletal target facts are missing or incompatible.";
				return false;
			}
			if (!ParseBuildTargetFactUInt32(*Bones, OutContext.SkeletonBoneCount)
				|| !ParseBuildTargetFactUInt32(*Materials, OutContext.MaterialSlotCount))
			{
				OutError = "Skeletal target facts are malformed.";
				return false;
			}
			OutContext.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64;
			OutContext.TargetProfile = ESkeletalPayloadTargetProfile::Game;
			return OutContext.SkeletonBoneCount != 0;
		}

		template<typename T>
		class TSkeletalBuildFunction final : public IBuildFunction
		{
		public:
			TSkeletalBuildFunction(
				std::string InRoot, std::string InInputName, uint64 InMaximumBytes)
				: Root(std::move(InRoot)), InputName(std::move(InInputName)),
				  MaximumBytes(InMaximumBytes)
			{
			}

			auto GetConfig() const -> FBuildFunctionConfig override
			{
				return {.CacheBucket = Root,
					.ExpectedValueName = std::string(SkeletalValueName),
					.MaximumValueBytes = MaximumBytes,
					.CleanupBudgetBytes = SkeletalDerivedDataBudgetBytes,
					.CleanupDeleteLimit = SkeletalDerivedDataCleanupDeleteLimit};
			}

			auto Validate(const FBuildDefinition& Definition, const FBuildValue& Value,
				std::string& OutError) const -> bool override
			{
				FSkeletalPayloadSerializationContext Context;
				T Payload;
				if (!ContextFromDefinition(Definition, Context, OutError)
					|| !DecodePayload(Value, Definition.GetKey().ToString(),
						Context, Payload, OutError)) return false;
				const auto Fingerprint = Definition.GetTargetFact("PayloadFingerprint");
				if (Fingerprint
					&& FXxHash128::HashBuffer(Value.GetBytes()).ToString() != *Fingerprint)
				{
					OutError = "Skeletal payload fingerprint is incompatible.";
					return false;
				}
				return true;
			}

			auto Build(const FBuildContext& Context, FBuildValue& OutValue,
				std::string& OutError) const -> bool override
			{
				const FBuildValue* Input = Context.GetInput(InputName);
				if (!Input)
				{
					OutError = "Skeletal local build input is missing.";
					return false;
				}
				const auto Fingerprint =
					Context.GetDefinition().GetTargetFact("PayloadFingerprint");
				if (!Fingerprint
					|| FXxHash128::HashBuffer(Input->GetBytes()).ToString() != *Fingerprint)
				{
					OutError = "Skeletal local input fingerprint is incompatible.";
					return false;
				}
				if (Context.IsCanceled())
				{
					OutError = "Skeletal build was canceled.";
					return false;
				}
				OutValue = FBuildValue::FromOwned(std::string(SkeletalValueName),
					std::vector<std::byte>(Input->GetBytes().begin(), Input->GetBytes().end()));
				return true;
			}

		private:
			std::string Root;
			std::string InputName;
			uint64 MaximumBytes = 0;
		};
	}

	auto EncodeSkeletalMeshPayload(
		FSkeletalMeshPayloadData& Payload,
		const FSkeletalPayloadSerializationContext& Context,
		std::vector<std::byte>& OutBytes,
		std::string& OutError) -> bool
	{
		return EncodePayload(Payload, Context, OutBytes, OutError);
	}

	auto EncodeAnimationClipPayload(
		FAnimationClipPayloadData& Payload,
		const FSkeletalPayloadSerializationContext& Context,
		std::vector<std::byte>& OutBytes,
		std::string& OutError) -> bool
	{
		return EncodePayload(Payload, Context, OutBytes, OutError);
	}

	auto DecodeSkeletalMeshPayload(
		const FBuildValue& Value, std::string_view Key,
		const FSkeletalPayloadSerializationContext& Context,
		FSkeletalMeshPayloadData& OutPayload,
		std::string& OutError) -> bool
	{
		return DecodePayload(Value, Key, Context, OutPayload, OutError);
	}

	auto DecodeAnimationClipPayload(
		const FBuildValue& Value, std::string_view Key,
		const FSkeletalPayloadSerializationContext& Context,
		FAnimationClipPayloadData& OutPayload,
		std::string& OutError) -> bool
	{
		return DecodePayload(Value, Key, Context, OutPayload, OutError);
	}

	auto CreateSkeletalMeshBuildFunction() -> std::shared_ptr<IBuildFunction>
	{
		return std::make_shared<TSkeletalBuildFunction<FSkeletalMeshPayloadData>>(
			"SkeletalMesh/Objects", std::string(SkeletalMeshInputName),
			MaximumSkeletalMeshPayloadBytes);
	}

	auto CreateAnimationClipBuildFunction() -> std::shared_ptr<IBuildFunction>
	{
		return std::make_shared<TSkeletalBuildFunction<FAnimationClipPayloadData>>(
			"AnimationClip/Objects", std::string(AnimationClipInputName),
			MaximumAnimationClipPayloadBytes);
	}
}
