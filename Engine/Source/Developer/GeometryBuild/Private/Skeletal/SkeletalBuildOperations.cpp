#include "Skeletal/SkeletalBuildOperations.h"

#include "AssetBuild/BuildSession.h"
#include "Serialization/Archive.h"

namespace Durin::Asset::Build
{
	namespace
	{
		inline constexpr uint64 SkeletalDerivedDataBudgetBytes =
			32ull * 1024ull * 1024ull * 1024ull;
		inline constexpr uint32 SkeletalDerivedDataCleanupDeleteLimit = 256;
		const FBuildFunctionIdentity SkeletalMeshFunctionIdentity{
			"Durin.GeometryBuild.SkeletalMesh", 1};
		const FBuildFunctionIdentity AnimationClipFunctionIdentity{
			"Durin.GeometryBuild.AnimationClip", 1};
		constexpr std::string_view SkeletalMeshInputName = "SkeletalMeshBuildInput";
		constexpr std::string_view AnimationClipInputName = "AnimationClipBuildInput";
		constexpr std::string_view SkeletalValueName = "SkeletalPayload";

		auto ParseU32(std::string_view Text, uint32& OutValue) -> bool
		{
			if (Text.empty()) return false;
			uint64 Value = 0;
			for (const char Character : Text)
			{
				if (Character < '0' || Character > '9') return false;
				Value = Value * 10 + static_cast<uint32>(Character - '0');
				if (Value > std::numeric_limits<uint32>::max()) return false;
			}
			OutValue = static_cast<uint32>(Value);
			return true;
		}

		auto ValidateKeyFields(FArchive& Ar, const FSkeletalBuildKeyFields& Input) -> bool
		{
			if (Ar.IsLoading())
			{
				Ar.Fail(EArchiveFailureCode::UnsupportedCapability,
					"Skeletal build-key input is save-only.");
				return false;
			}
			if (Input.ProviderIdentity.empty() || Input.ProviderVersion == 0
				|| Input.StableOutputIdentity.empty()
				|| Input.SkeletonCompatibilityIdentity.empty()
				|| Input.TargetPlatform != ESkeletalPayloadTargetPlatform::Win64
				|| Input.TargetProfile != ESkeletalPayloadTargetProfile::Game)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"Skeletal build-key identity is incomplete or unsupported.");
				return false;
			}
			return true;
		}

		auto SerializeKeyFields(
			FArchive& Ar,
			FSkeletalBuildKeyFields& Input,
			std::string_view BuilderIdentity,
			uint32 BuilderVersion,
			uint32 PayloadVersion) -> void
		{
			if (!ValidateKeyFields(Ar, Input)) return;
			uint32 KeyVersion = SkeletalPayloadKeySchemaVersion;
			std::string Identity(BuilderIdentity);
			uint32 Platform = static_cast<uint32>(Input.TargetPlatform);
			uint32 Profile = static_cast<uint32>(Input.TargetProfile);
			auto SerializeVersionOneString = [&Ar](std::string& Value) {
				if (Value.size() > std::numeric_limits<uint32>::max())
				{
					Ar.Fail(EArchiveFailureCode::LimitExceeded,
						"Skeletal build-key string exceeds the version-1 wire bound.");
					return;
				}
				uint32 Size = static_cast<uint32>(Value.size());
				Ar << Size;
				if (!Ar.HasError()) Ar.Serialize(Value.data(), Value.size());
			};
			Ar << KeyVersion;
			SerializeVersionOneString(Identity);
			Ar << BuilderVersion << PayloadVersion << Platform << Profile;
			SerializeVersionOneString(Input.ProviderIdentity);
			Ar << Input.ProviderVersion
				<< Input.SourceClosureHash.HashLow << Input.SourceClosureHash.HashHigh
				<< Input.SettingsHash.HashLow << Input.SettingsHash.HashHigh
				<< Input.ProviderStateHash.HashLow << Input.ProviderStateHash.HashHigh
				<< Input.PayloadInputFingerprint.HashLow
				<< Input.PayloadInputFingerprint.HashHigh;
			SerializeVersionOneString(Input.StableOutputIdentity);
			SerializeVersionOneString(Input.SkeletonCompatibilityIdentity);
		}

		template<typename T>
		auto MakeKeyBytes(const T& Input, std::string& OutError) -> std::vector<uint8>
		{
			std::vector<uint8> Bytes;
			FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataKey);
			const_cast<T&>(Input).Serialize(Ar);
			if (Ar.HasError())
			{
				OutError = Ar.GetFailure()->Message;
				Bytes.clear();
			}
			else OutError.clear();
			return Bytes;
		}

		template<typename T>
		auto SerializePayload(
			T& Payload,
			const FSkeletalPayloadSerializationContext& Context,
			std::vector<uint8>& OutBytes,
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
		auto DecodePayload(
			const FBuildValue& Value,
			std::string_view Key,
			const FSkeletalPayloadSerializationContext& Context,
			T& OutPayload,
			std::string& OutMessage) -> bool
		{
			if (Value.GetName() != SkeletalValueName)
			{
				OutMessage = std::format("Skeletal value name for key {} is incompatible.", Key);
				return false;
			}
			T Candidate;
			FCanonicalMemoryReader Ar(Value.GetBytes(), EArchivePurpose::DerivedDataPayload);
			Candidate.Serialize(Ar, Context);
			if (Ar.HasError() || !RequireArchiveEnd(Ar))
			{
				OutMessage = Ar.GetFailure() ? Ar.GetFailure()->Message
					: "Skeletal payload has trailing bytes.";
				return false;
			}
			OutPayload = std::move(Candidate);
			OutMessage.clear();
			return true;
		}

		auto ContextFromDefinition(const FBuildDefinition& Definition,
			FSkeletalPayloadSerializationContext& OutContext, std::string& OutError) -> bool
		{
			const auto Bones = Definition.GetTargetFact("SkeletonBoneCount");
			const auto Materials = Definition.GetTargetFact("MaterialSlotCount");
			const auto Platform = Definition.GetTargetFact("Platform");
			const auto Profile = Definition.GetTargetFact("Profile");
			if (!Bones || !Materials || Platform != std::optional<std::string_view>("Win64")
				|| Profile != std::optional<std::string_view>("Game"))
			{
				OutError = "Skeletal target facts are missing or incompatible.";
				return false;
			}
			if (!ParseU32(*Bones, OutContext.SkeletonBoneCount)
				|| !ParseU32(*Materials, OutContext.MaterialSlotCount))
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
			TSkeletalBuildFunction(std::string InRoot, std::string InInputName, uint64 InMaximumBytes)
				: Root(std::move(InRoot)), InputName(std::move(InInputName)), MaximumBytes(InMaximumBytes) {}
			auto GetConfig() const -> FBuildFunctionConfig override
			{
				return {.CacheRoot = Root, .ExpectedValueName = std::string(SkeletalValueName),
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
					|| !DecodePayload(Value, Definition.GetKey().ToString(), Context, Payload, OutError))
					return false;
				const auto Fingerprint = Definition.GetTargetFact("PayloadFingerprint");
				if (Fingerprint && FXxHash128::HashBuffer(Value.GetBytes()).ToString() != *Fingerprint)
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
				if (!Input) { OutError = "Skeletal local build input is missing."; return false; }
				const auto Fingerprint = Context.GetDefinition().GetTargetFact("PayloadFingerprint");
				if (!Fingerprint || FXxHash128::HashBuffer(Input->GetBytes()).ToString() != *Fingerprint)
				{
					OutError = "Skeletal local input fingerprint is incompatible.";
					return false;
				}
				if (Context.IsCanceled()) { OutError = "Skeletal build was canceled."; return false; }
				OutValue = FBuildValue::FromOwned(std::string(SkeletalValueName),
					std::vector<uint8>(Input->GetBytes().begin(), Input->GetBytes().end()));
				return true;
			}
		private:
			std::string Root;
			std::string InputName;
			uint64 MaximumBytes = 0;
		};

		std::mutex GSkeletalFunctionMutex;
		FBuildFunctionRegistration GSkeletalMeshFunctionRegistration;
		FBuildFunctionRegistration GAnimationClipFunctionRegistration;
		auto EnsureSkeletalBuildFunctions(std::string* OutError,
			FModuleOwnedCallbackGate Gate = {}) -> bool
		{
			std::lock_guard Lock(GSkeletalFunctionMutex);
			if (GSkeletalMeshFunctionRegistration.IsValid()
				&& GAnimationClipFunctionRegistration.IsValid()) return true;
			GSkeletalMeshFunctionRegistration = RegisterBuildFunction(SkeletalMeshFunctionIdentity,
				std::make_shared<TSkeletalBuildFunction<FSkeletalMeshPayloadData>>(
					"SkeletalMesh/Objects", std::string(SkeletalMeshInputName), MaximumSkeletalMeshPayloadBytes),
				Gate, OutError);
			if (!GSkeletalMeshFunctionRegistration.IsValid()) return false;
			GAnimationClipFunctionRegistration = RegisterBuildFunction(AnimationClipFunctionIdentity,
				std::make_shared<TSkeletalBuildFunction<FAnimationClipPayloadData>>(
					"AnimationClip/Objects", std::string(AnimationClipInputName), MaximumAnimationClipPayloadBytes),
				std::move(Gate), OutError);
			if (!GAnimationClipFunctionRegistration.IsValid())
			{
				GSkeletalMeshFunctionRegistration.Reset();
				return false;
			}
			return true;
		}

		template<typename T>
		auto ExecuteSkeletalSession(const FBuildFunctionIdentity& Identity,
			std::string_view InputName, std::string_view Key,
			std::span<const uint8> KeyBytes, std::span<const uint8> LocalBytes,
			const FSkeletalPayloadSerializationContext& Context,
			std::string_view SkeletonIdentity, bool bRequireStore,
			FBuildOutput& OutOutput, T& OutPayload, std::string& OutError) -> bool
		{
			if (!EnsureSkeletalBuildFunctions(&OutError)) return false;
			FBuildDefinition Definition;
			FBuildDefinitionBuilder Builder(Identity, std::string(SkeletalValueName));
			Builder.SetKey(FBuildKey::FromString(Key), KeyBytes)
				.AddTargetFact("Platform", "Win64").AddTargetFact("Profile", "Game")
				.AddTargetFact("SkeletonBoneCount", std::to_string(Context.SkeletonBoneCount))
				.AddTargetFact("MaterialSlotCount", std::to_string(Context.MaterialSlotCount));
			if (!SkeletonIdentity.empty()) Builder.AddTargetFact("SkeletonIdentity", std::string(SkeletonIdentity));
			if (!LocalBytes.empty())
			{
				Builder.AddTargetFact("PayloadFingerprint", FXxHash128::HashBuffer(LocalBytes).ToString())
					.AddInput(FBuildValue::FromOwned(std::string(InputName),
						std::vector<uint8>(LocalBytes.begin(), LocalBytes.end())));
			}
			if (!Builder.Build(Definition, &OutError)) return false;
			OutOutput = FBuildSession().Build(Definition, {.bQueryCache = true,
				.bAllowLocalBuild = !LocalBytes.empty(), .bStoreBuildResult = !LocalBytes.empty(),
				.bRequireStoreSuccess = bRequireStore, .bReturnData = true});
			if (!OutOutput.Succeeded()) { OutError = OutOutput.Diagnostic; return false; }
			return DecodePayload(OutOutput.Value, Key, Context, OutPayload, OutError);
		}
	}

	auto FSkeletalMeshBuildKeyInput::Serialize(FArchive& Ar) -> void
	{
		SerializeKeyFields(Ar, *this, SkeletalMeshBuilderIdentity,
			SkeletalMeshBuilderVersion, SkeletalMeshPayloadSchemaVersion);
	}

	auto FAnimationClipBuildKeyInput::Serialize(FArchive& Ar) -> void
	{
		SerializeKeyFields(Ar, *this, AnimationClipBuilderIdentity,
			AnimationClipBuilderVersion, AnimationClipPayloadSchemaVersion);
	}

	auto BuildSkeletalMeshDerivedDataKeyBytes(
		const FSkeletalMeshBuildKeyInput& Input,
		std::string& OutError) -> std::vector<uint8>
	{
		return MakeKeyBytes(Input, OutError);
	}

	auto BuildSkeletalMeshDerivedDataKey(
		const FSkeletalMeshBuildKeyInput& Input,
		std::string& OutError) -> std::string
	{
		const std::vector<uint8> Bytes = MakeKeyBytes(Input, OutError);
		return Bytes.empty() ? std::string{} : FXxHash128::HashBuffer(Bytes).ToString();
	}

	auto BuildAnimationClipDerivedDataKeyBytes(
		const FAnimationClipBuildKeyInput& Input,
		std::string& OutError) -> std::vector<uint8>
	{
		return MakeKeyBytes(Input, OutError);
	}

	auto BuildAnimationClipDerivedDataKey(
		const FAnimationClipBuildKeyInput& Input,
		std::string& OutError) -> std::string
	{
		const std::vector<uint8> Bytes = MakeKeyBytes(Input, OutError);
		return Bytes.empty() ? std::string{} : FXxHash128::HashBuffer(Bytes).ToString();
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
		std::vector<uint8> Bytes;
		FSkeletalMeshPayloadData& Payload =
			const_cast<FSkeletalMeshPayloadData&>(*Request.Payload);
		if (!SerializePayload(Payload, Context, Bytes, OutError)) return false;
		Request.KeyInput.PayloadInputFingerprint = FXxHash128::HashBuffer(Bytes);
		const std::string Key = BuildSkeletalMeshDerivedDataKey(Request.KeyInput, OutError);
		if (Key.empty()) return false;
		const std::vector<uint8> KeyBytes = BuildSkeletalMeshDerivedDataKeyBytes(Request.KeyInput, OutError);
		FBuildOutput Output;
		FSkeletalMeshPayloadData SelectedPayload;
		if (!ExecuteSkeletalSession(SkeletalMeshFunctionIdentity, SkeletalMeshInputName,
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
		std::vector<uint8> Bytes;
		FAnimationClipPayloadData& Payload =
			const_cast<FAnimationClipPayloadData&>(*Request.Payload);
		if (!SerializePayload(Payload, Context, Bytes, OutError)) return false;
		Request.KeyInput.PayloadInputFingerprint = FXxHash128::HashBuffer(Bytes);
		const std::string Key = BuildAnimationClipDerivedDataKey(Request.KeyInput, OutError);
		if (Key.empty()) return false;
		const std::vector<uint8> KeyBytes = BuildAnimationClipDerivedDataKeyBytes(Request.KeyInput, OutError);
		FBuildOutput Output;
		FAnimationClipPayloadData SelectedPayload;
		if (!ExecuteSkeletalSession(AnimationClipFunctionIdentity, AnimationClipInputName,
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
		const bool bLoaded = ExecuteSkeletalSession(SkeletalMeshFunctionIdentity,
			SkeletalMeshInputName, Key, {}, {}, Context, {}, false,
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
		const bool bLoaded = ExecuteSkeletalSession(AnimationClipFunctionIdentity,
			AnimationClipInputName, Key, {}, {}, Context, {}, false,
			Output, OutPayload, Error);
		OutMessage = bLoaded ? Output.Diagnostic : std::move(Error);
		return bLoaded;
	}

	auto InitializeSkeletalBuildFunctions(FModuleOwnedCallbackGate Gate,
		std::string* OutError) -> bool
	{
		return EnsureSkeletalBuildFunctions(OutError, std::move(Gate));
	}

	auto ShutdownSkeletalBuildFunctions() -> void
	{
		std::lock_guard Lock(GSkeletalFunctionMutex);
		GAnimationClipFunctionRegistration.Reset();
		GSkeletalMeshFunctionRegistration.Reset();
	}
}
