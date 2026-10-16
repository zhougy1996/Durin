#include "Materials/MaterialProgramCompiler.h"
#include "MaterialPreparedProgram.h"
#include "MaterialBuildVersion.h"
#include "Hash/XxHash.h"
#include "Serialization/BinaryFormat.h"
#include <glm/detail/setup.hpp>

#include "Materials/MaterialRenderTypes.h"
#include "Shader/ShaderCompilerCore.h"
#include "Shader/IShaderBuildModule.h"
#include "DynamicRHI.h"
#include "RHIGlobals.h"
#if DURIN_WITH_EDITOR
#include "TargetPlatform.h"
#endif

namespace Durin
{
	auto GetMaterialBuildVersion() -> uint64
	{
		static const uint64 Version = [] {
			FBinaryWriter Writer;
			Writer.WriteString("Durin.Engine.MaterialProgram");
			Writer.WriteU32(MaterialBuildPrivate::NormalizationAlgorithmRevision);
			Writer.WriteU32(MaterialBuildPrivate::GeneratorAlgorithmRevision);
			Writer.WriteU32(GLM_VERSION_MAJOR);
			Writer.WriteU32(GLM_VERSION_MINOR);
			Writer.WriteU32(GLM_VERSION_PATCH);
			const uint64 Hash = FXxHash64::HashBuffer(Writer.TakeBytes()).HashValue;
			return Hash ? Hash : uint64{1};
		}();
		return Version;
	}

	auto GetMaterialCompileAdapterVersion() -> uint32
	{
		return MaterialBuildPrivate::CompileAdapterVersion;
	}

	namespace
	{
		template <typename TValue, bool = std::is_enum_v<TValue>>
		struct TCanonicalIntegerType
		{
			using Type = TValue;
		};

		template <typename TValue>
		struct TCanonicalIntegerType<TValue, true>
		{
			using Type = std::underlying_type_t<TValue>;
		};

		constexpr std::array GSurfaceOutputOrder{
			EMaterialSurfaceOutput::BaseColor,
			EMaterialSurfaceOutput::Normal,
			EMaterialSurfaceOutput::Metallic,
			EMaterialSurfaceOutput::Roughness,
			EMaterialSurfaceOutput::AmbientOcclusion,
			EMaterialSurfaceOutput::Emissive,
			EMaterialSurfaceOutput::Opacity,
			EMaterialSurfaceOutput::OpacityMask};
		inline constexpr uint32 MaterialProgramIdentitySchemaVersion = 8;

		auto IsCommutative(EMaterialProgramOpcode Opcode) -> bool
		{
			return Opcode == EMaterialProgramOpcode::Add
				|| Opcode == EMaterialProgramOpcode::Multiply
				|| Opcode == EMaterialProgramOpcode::Minimum
				|| Opcode == EMaterialProgramOpcode::Maximum
				|| Opcode == EMaterialProgramOpcode::Dot
				|| Opcode == EMaterialProgramOpcode::Distance;
		}

		template <typename TValue>
		auto AppendLittleEndian(FByteBuffer& Bytes, TValue Value)
			-> void
			requires std::is_integral_v<TValue> || std::is_enum_v<TValue>
		{
			using TRaw = typename TCanonicalIntegerType<TValue>::Type;
			using TUnsigned = std::make_unsigned_t<TRaw>;
			static_assert(sizeof(TUnsigned) <= sizeof(uint64));
			uint64 Raw = static_cast<TUnsigned>(Value);
			for (size_t Index = 0; Index < sizeof(TUnsigned); ++Index)
			{
				Bytes.push_back(static_cast<std::byte>(Raw & 0xffu));
				Raw >>= 8u;
			}
		}

		auto AppendBytes(
			FByteBuffer& Bytes,
			FByteView Value) -> void
		{
			AppendLittleEndian(Bytes, static_cast<uint64>(Value.size()));
			Bytes.insert(Bytes.end(), Value.begin(), Value.end());
		}

		auto AppendString(
			FByteBuffer& Bytes,
			std::string_view Value) -> void
		{
			AppendLittleEndian(Bytes, static_cast<uint64>(Value.size()));
			Bytes.insert(Bytes.end(),
				reinterpret_cast<const std::byte*>(Value.data()),
				reinterpret_cast<const std::byte*>(Value.data() + Value.size()));
		}

		auto AppendGuid(FByteBuffer& Bytes, const FGuid& Guid) -> void
		{
			AppendLittleEndian(Bytes, Guid.A);
			AppendLittleEndian(Bytes, Guid.B);
			AppendLittleEndian(Bytes, Guid.C);
			AppendLittleEndian(Bytes, Guid.D);
		}

		auto CanonicalFloatBits(float Value) -> uint32
		{
			return Value == 0.0f ? 0u : std::bit_cast<uint32>(Value);
		}

		auto AppendLiteral(
			FByteBuffer& Bytes,
			const FMaterialProgramLiteral& Literal) -> void
		{
			AppendLittleEndian(Bytes, CanonicalFloatBits(Literal.X));
			AppendLittleEndian(Bytes, CanonicalFloatBits(Literal.Y));
			AppendLittleEndian(Bytes, CanonicalFloatBits(Literal.Z));
			AppendLittleEndian(Bytes, CanonicalFloatBits(Literal.W));
		}

		auto AppendIRNode(
			FByteBuffer& Bytes,
			const MIR::FNode& Node) -> void
		{
			AppendLittleEndian(Bytes, Node.Opcode);
			AppendLittleEndian(Bytes, Node.ResultType);
			AppendLittleEndian(Bytes, Node.LegalStages);
			AppendLittleEndian(Bytes, Node.SpatialKind);
			AppendLittleEndian(Bytes, Node.CoordinateSpace);
			AppendLittleEndian(Bytes, Node.ScalarBroadcastMask);
			AppendLittleEndian(Bytes, static_cast<uint32>(Node.Inputs.size()));
			for (uint32 Input : Node.Inputs) AppendLittleEndian(Bytes, Input);
			AppendLiteral(Bytes, Node.GetLiteral());
			AppendGuid(Bytes, Node.GetParameterId());
			AppendLittleEndian(Bytes, Node.GetSwizzle().Length);
			AppendLittleEndian(Bytes, Node.GetSwizzle().Components[0]);
			AppendLittleEndian(Bytes, Node.GetSwizzle().Components[1]);
			AppendLittleEndian(Bytes, Node.GetSwizzle().Components[2]);
			AppendLittleEndian(Bytes, Node.GetSwizzle().Components[3]);
			AppendGuid(Bytes, Node.GetCollectionParameter().CollectionId);
			AppendGuid(Bytes, Node.GetCollectionParameter().ParameterId);
			AppendLittleEndian(Bytes, Node.GetTransform().Source);
			AppendLittleEndian(Bytes, Node.GetTransform().Destination);
			if (const auto* Read = std::get_if<MIR::FCustomDataRead>(&Node.Payload))
				AppendLittleEndian(Bytes, Read->Index);
		}

		auto MakeNormalizationFailure(FMaterialError Error)
			-> FMaterialProgramDiagnostic
		{
			return {
				.Category = EMaterialProgramDiagnosticCategory::Normalization,
				.LocationKind =
					EMaterialProgramDiagnosticLocationKind::Program,
				.Error = std::move(Error)};
		}
	}

	auto MakeMaterialShaderCompileOptions(FShaderTargetIdentity Target, const FMaterialStaticProperties& Properties) -> FShaderCompileOptions
	{
		FShaderCompileOptions Options; Options.Target = Target;
		for (const auto& Entry : MaterialShaderEntries)
		{
			Options.EntryPoints.push_back(Entry.Name.data());
			Options.Frequencies.push_back(Entry.Frequency);
		}
		Options.Macros.emplace_back("DURIN_MATERIAL_BLEND_MODE", std::to_string(static_cast<uint8>(Properties.BlendMode)));
		Options.Macros.emplace_back("DURIN_MATERIAL_SHADING_MODEL", std::to_string(static_cast<uint8>(Properties.ShadingModel)));
		Options.Macros.emplace_back("DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS", std::to_string(std::bit_cast<uint32>(Properties.OpacityMaskThreshold)));
		return Options;
	}

	auto BuildDefaultMaterialCompilerEnvironment(
		FMaterialCompilerEnvironment& OutEnvironment,
		std::optional<FShaderTargetIdentity> Target) -> FMaterialOperationResult
	{
		FShaderCompileOptions Options;
		const auto SelectedTarget = Target.value_or(Options.Target);
#if DURIN_WITH_EDITOR
		const auto Platform = ToCookTargetPlatform(SelectedTarget.Platform);
		const auto Resolved = Platform ? ResolveTargetPlatform(*Platform)
			: std::expected<FTargetPlatformDescription, ETargetPlatformError>{std::unexpected(ETargetPlatformError::UnknownPlatform)};
		if (!Resolved || Resolved->GetShaderTarget() != SelectedTarget) return {FMaterialError::FromExternal(EMaterialCompileError::ShaderEnvironmentUnavailable,
			"Material shader target is unsupported.")};
#endif
		const bool bMetal = SelectedTarget == MetalShaderTarget;
		Options = MakeMaterialShaderCompileOptions(SelectedTarget, {.OpacityMaskThreshold = 0.5f});
		FShaderSourceDependencyFingerprint SourceTree;
		FShaderOperationResult ProviderError;
		if (!(ProviderError = BuildShaderSourceTreeFingerprint("/Engine/MaterialCompilerEnvironment", Options, SourceTree)))
		{
			return {FMaterialError::FromExternal(EMaterialCompileError::ShaderEnvironmentUnavailable, FormatShaderError(ProviderError.error()))};
		}

		FMaterialCompilerEnvironment Environment;
		Environment.Target = bMetal ? "metal-msl-2.0" : "vulkan-spirv-1.5";
		if (const auto* Capabilities = !Target && GDynamicRHI ? GDynamicRHI->RHIGetCapabilities() : nullptr)
		{
			Environment.FeatureLevel = Capabilities->FeatureLevel;
			auto& Limits = Environment.ResourceLimits;
			Limits.SampledImages = std::min(Limits.SampledImages, Capabilities->MaxFragmentSampledImages);
			Limits.Samplers = std::min(Limits.Samplers, Capabilities->MaxFragmentSamplers);
			Limits.UniformBuffers = std::min(Limits.UniformBuffers, Capabilities->MaxFragmentUniformBuffers);
			Limits.StageResources = std::min(Limits.StageResources, Capabilities->MaxFragmentResources);
			Limits.UniformBufferBytes = std::min(Limits.UniformBufferBytes, Capabilities->MaxUniformBufferRange);
		}
		const auto* ShaderBuild = IShaderBuildModule::Get();
		const uint64 ShaderBuildVersion = ShaderBuild ? ShaderBuild->GetBuildVersion() : 0;
		if (!ShaderBuildVersion)
			return {FMaterialError::FromExternal(EMaterialCompileError::ShaderEnvironmentUnavailable,
				"Shader build identity is unavailable.")};
		const auto Adapter = ShaderBuild->GetCompileAdapterVersion();
		Environment.CompilerIdentity = std::format("ShaderBuild:{:016x};adapter={}:{}:{}",
			ShaderBuildVersion, Adapter.FunctionVersion, Adapter.ConstantsSchema, Adapter.OutputSchema);
		Environment.Dependencies.push_back({
			.VirtualPath = std::move(SourceTree.VirtualPath),
			.ContentHash = SourceTree.ContentHash});
		OutEnvironment = std::move(Environment);
		return {};
	}

	auto ValidateMaterialNormalizationEnvironment(const MIR::FCompilerInput& Input) -> MIR::FNormalizationResult
	{
		MIR::FNormalizationResult Result;
		const auto StaticPropertiesError = ValidateMaterialStaticProperties(
			Input.StaticProperties);
		if (!StaticPropertiesError)
		{
			Result.Diagnostics.push_back(MakeNormalizationFailure(
				std::move(StaticPropertiesError.Error)));
			return Result;
		}
		if (Input.Environment.CompilerIdentity.empty()
			|| Input.Environment.BuilderVersion == 0
			|| Input.Environment.AdapterVersion == 0
			|| Input.Environment.Target.empty()
			|| Input.Environment.CompilerIdentity.size()
				> MaterialProgramMaxStringBytes
			|| Input.Environment.Target.size()
				> MaterialProgramMaxStringBytes
			|| Input.Environment.PassContractVersion == 0
			|| Input.Environment.Dependencies.size() > 64)
		{
			Result.Diagnostics.push_back(MakeNormalizationFailure(
				EMaterialIRError::InvalidCompilerEnvironment));
			return Result;
		}
		if (!IsSupportedMaterialQualityLevel(Input.Environment.Quality)
			|| !IsSupportedMaterialFeatureLevel(Input.Environment.FeatureLevel)
			|| Input.Environment.StaticBools.size() > MaterialMaxStaticBoolDeclarations
			|| !std::ranges::is_sorted(Input.Environment.StaticBools, {},
				&FMaterialCompilerEnvironment::FStaticBoolValue::DeclarationId))
		{
			Result.Diagnostics.push_back(MakeNormalizationFailure(
				EMaterialIRError::InvalidCompilerEnvironment));
			return Result;
		}
		for (size_t Index = 0; Index < Input.Environment.StaticBools.size(); ++Index)
			if (!Input.Environment.StaticBools[Index].DeclarationId.IsValid()
				|| (Index && Input.Environment.StaticBools[Index - 1].DeclarationId
					== Input.Environment.StaticBools[Index].DeclarationId))
			{
				Result.Diagnostics.push_back(MakeNormalizationFailure(
					EMaterialIRError::InvalidCompilerEnvironment));
				return Result;
			}
		for (size_t Index = 0;
			Index < Input.Environment.Dependencies.size(); ++Index)
		{
			const auto& Dependency = Input.Environment.Dependencies[Index];
			if (Dependency.VirtualPath.empty()
				|| Dependency.VirtualPath.size() > MaterialProgramMaxStringBytes
				|| !Dependency.VirtualPath.starts_with('/')
				|| Dependency.VirtualPath.find('\\') != std::string::npos
				|| Dependency.VirtualPath.find(':') != std::string::npos
				|| Dependency.VirtualPath.find("//") != std::string::npos
				|| Dependency.VirtualPath.find("/../") != std::string::npos
				|| Dependency.VirtualPath.ends_with("/..")
				|| Dependency.VirtualPath.find("/./") != std::string::npos
				|| Dependency.VirtualPath.ends_with("/.")
				|| Dependency.ContentHash.IsZero())
			{
				Result.Diagnostics.push_back(MakeNormalizationFailure(
					EMaterialIRError::CompilerDependencyManifestContainsInvalidEntry));
				return Result;
			}
			for (size_t Other = 0; Other < Index; ++Other)
				if (Input.Environment.Dependencies[Other].VirtualPath
					== Dependency.VirtualPath)
				{
					Result.Diagnostics.push_back(MakeNormalizationFailure(
						EMaterialIRError::CompilerDependencyManifestContainsDuplicateVirtualPath));
					return Result;
				}
		}

		Result.bSucceeded = true;
		return Result;
	}

	auto MIR::Normalize(const MIR::FCompilerInput& Input) -> MIR::FNormalizationResult
	{
		MIR::FNormalizationResult Result;
		const auto EnvironmentValidation = ValidateMaterialNormalizationEnvironment(Input);
		if (!EnvironmentValidation) return EnvironmentValidation;
		if (Input.Collections.size() > MaterialParameterCollectionMaxPerMaterial)
		{
			Result.Diagnostics.push_back(MakeNormalizationFailure(
				EMaterialIRError::ParameterDeclarationCountExceedsBound));
			return Result;
		}
		if (Input.Collections.size() + 5 > Input.Environment.ResourceLimits.UniformBuffers)
		{
			Result.Diagnostics.push_back(MakeNormalizationFailure(
				EMaterialIRError::ParameterDeclarationCountExceedsBound));
			return Result;
		}
		std::unordered_set<FGuid> CollectionIds;
		for (const auto& Collection : Input.Collections)
			if (!Collection.CollectionId.IsValid()
				|| Collection.SchemaVersion != CurrentMaterialParameterCollectionSchemaVersion
				|| !CollectionIds.insert(Collection.CollectionId).second
				|| !ValidateCompiledMaterialLayout(Collection.UniformLayout,
					Input.Environment.ResourceLimits))
			{
				Result.Diagnostics.push_back(MakeNormalizationFailure(
					EMaterialIRError::ParameterDeclarationInvalidDuplicated));
				return Result;
			}
		auto Validation = MIR::Validate(Input.IR, Input.Parameters);
		if (!Validation) { Result.Diagnostics = std::move(Validation.Diagnostics); return Result; }
		if (Input.Sources.size() > MaterialFunctionMaxExpandedNodes)
		{
			Result.Diagnostics.push_back(MakeNormalizationFailure(EMaterialIRError::SourceMetadataCountExceedsBound));
			return Result;
		}
		uint64 SourceBytes = 0;
		for (const auto& Source : Input.Sources)
		{
			SourceBytes += sizeof(Source) + Source.FunctionAssetPath.size() + Source.CallPath.size() * sizeof(FGuid);
			if (Source.ExpressionIndex >= Input.IR.Nodes.size() || Source.CallPath.size() > MaterialFunctionMaxCallDepth
				|| Source.FunctionAssetPath.size() > MaterialProgramMaxStringBytes || SourceBytes > MaterialFunctionMaxClosureBytes
				|| (Source.InputIndex && *Source.InputIndex >= MaterialProgramMaxNodeInputCount)
				|| (Source.UVFieldIndex && *Source.UVFieldIndex >= 4))
			{
				Result.Diagnostics.push_back(MakeNormalizationFailure(EMaterialIRError::SourceMetadataInvalidExceedsBound));
				return Result;
			}
		}
		auto Nodes = Input.IR.Nodes;
		// Immediates outside the selected width cannot affect code identity.
		for (auto& Node : Nodes)
		{
			if (Node.Opcode == EMaterialProgramOpcode::Constant)
			{
				auto Literal = Node.GetLiteral();
				const std::array Components{&Literal.X, &Literal.Y, &Literal.Z, &Literal.W};
				for (uint32 Index = static_cast<uint32>(Node.ResultType) + 1; Index < 4; ++Index) *Components[Index] = 0.f;
				Node.Payload = Literal;
			}
			if (Node.Opcode == EMaterialProgramOpcode::Swizzle)
			{
				auto Swizzle = Node.GetSwizzle();
				for (uint32 Index = Swizzle.Length; Index < 4; ++Index) Swizzle.Components[Index] = 0;
				Node.Payload = Swizzle;
			}
		}
		struct FStructuralKey
		{
			FByteBuffer Header;
			std::vector<uint32> Inputs;
		};
		const auto Count = Nodes.size();
		std::vector<FStructuralKey> Keys(Count);
		// Memoization is optional: cap entries linearly even for adversarial DAGs.
		std::unordered_map<uint64, int8> Comparisons;
		const size_t MaxComparisonEntries = Count * 8;
		std::function<int8(uint32, uint32)> Compare = [&](uint32 A, uint32 B) -> int8 {
			if (A == B) return 0;
			const uint64 Pair = (uint64(std::min(A, B)) << 32) | std::max(A, B);
			if (const auto Cached = Comparisons.find(Pair); Cached != Comparisons.end())
				return A < B ? Cached->second : -Cached->second;
			const auto& Left = Keys[A];
			const auto& Right = Keys[B];
			int8 Order = Left.Header < Right.Header ? -1 : Left.Header > Right.Header ? 1 : 0;
			for (size_t Index = 0; Order == 0 && Index < std::min(Left.Inputs.size(), Right.Inputs.size()); ++Index)
				Order = Compare(Left.Inputs[Index], Right.Inputs[Index]);
			if (Order == 0) Order = Left.Inputs.size() < Right.Inputs.size() ? -1 : Left.Inputs.size() > Right.Inputs.size() ? 1 : 0;
			// Recursive calls can rehash; retain no map reference across them.
			if (Comparisons.size() < MaxComparisonEntries)
				Comparisons.emplace(Pair, A < B ? Order : -Order);
			return Order;
		};
		// Validation guarantees topological order, so all child keys already exist.
		for (size_t Index = 0; Index < Count; ++Index)
		{
			Keys[Index].Inputs = Nodes[Index].Inputs;
			if (IsCommutative(Nodes[Index].Opcode) && Keys[Index].Inputs.size() == 2
				&& Compare(Keys[Index].Inputs[1], Keys[Index].Inputs[0]) < 0)
			{
				std::swap(Keys[Index].Inputs[0], Keys[Index].Inputs[1]);
				// Operand metadata must follow the canonical operand permutation.
				const auto Mask = Nodes[Index].ScalarBroadcastMask;
				Nodes[Index].ScalarBroadcastMask = ((Mask & 1u) << 1) | ((Mask & 2u) >> 1);
			}
			auto Header = Nodes[Index];
			Header.Inputs.clear();
			AppendIRNode(Keys[Index].Header, Header);
		}
		constexpr uint32 InvalidIndex = 0xffffffffu;
		std::vector<uint32> Indices(Count, InvalidIndex);
		MIR::FModule IR;
		IR.SurfaceRoot = Input.IR.SurfaceRoot;
		IR.WorldPositionOffset = Input.IR.WorldPositionOffset;
		std::function<uint32(uint32)> Emit = [&](uint32 Index) -> uint32 {
			if (Indices[Index] != InvalidIndex) return Indices[Index];
			auto Node = Nodes[Index];
			Node.Inputs.clear();
			for (const auto Child : Keys[Index].Inputs) Node.Inputs.push_back(Emit(Child));
			const auto NewIndex = static_cast<uint32>(IR.Nodes.size());
			IR.Nodes.push_back(std::move(Node));
			Indices[Index] = NewIndex;
			return NewIndex;
		};
		if (IR.SurfaceRoot.bAggregate)
		{
			// Surface construction is lowered before normalization. Both authoring
			// modes share final-property filtering and reachability from this point.
			const auto& Surface = Nodes[IR.SurfaceRoot.AggregateExpressionIndex];
			for (size_t Index = 0; Index < IR.SurfaceRoot.Inputs.size(); ++Index)
			{
				auto& Root = IR.SurfaceRoot.Inputs[Index];
				Root.bExpression = true;
				Root.ExpressionIndex = Surface.Inputs[Index];
			}
			IR.SurfaceRoot.bAggregate = false;
		}
		{
			const FMaterialSurfaceOutputs Defaults;
			for (size_t Index = 0; Index < IR.SurfaceRoot.Inputs.size(); ++Index)
				if (!IsMaterialSurfaceOutputActive(static_cast<EMaterialSurfaceOutput>(Index), Input.StaticProperties))
				{
					auto& Root = IR.SurfaceRoot.Inputs[Index];
					Root.bExpression = false;
					Root.Literal = GetMaterialSurfaceOutputDefault(Defaults, static_cast<EMaterialSurfaceOutput>(Index));
				}

			IR.SurfaceRoot.AggregateExpressionIndex = 0;
			for (auto& Root : IR.SurfaceRoot.Inputs)
			{
				if (Root.bExpression) { Root.ExpressionIndex = Emit(Root.ExpressionIndex); Root.Literal = {}; }
				else
				{
					Root.ExpressionIndex = 0;
					const std::array Components{&Root.Literal.X, &Root.Literal.Y, &Root.Literal.Z, &Root.Literal.W};
					for (uint32 Index = static_cast<uint32>(Root.Type) + 1; Index < 4; ++Index) *Components[Index] = 0.f;
				}
			}
		}
		if (IR.WorldPositionOffset.bExpression)
		{
			IR.WorldPositionOffset.ExpressionIndex = Emit(IR.WorldPositionOffset.ExpressionIndex);
			IR.WorldPositionOffset.Literal = {};
		}
		else { IR.WorldPositionOffset.ExpressionIndex = 0; IR.WorldPositionOffset.Literal.W = 0.f; }
		for (const auto& Source : Input.Sources)
			if (Indices[Source.ExpressionIndex] != InvalidIndex)
			{
				auto Remapped = Source;
				Remapped.ExpressionIndex = Indices[Source.ExpressionIndex];
				Result.Sources.push_back(std::move(Remapped));
			}
		std::ranges::stable_sort(Result.Sources, {}, &MIR::FSource::ExpressionIndex);
		for (const auto& Node : IR.Nodes)
			if (Node.Opcode == EMaterialProgramOpcode::Parameter || Node.Opcode == EMaterialProgramOpcode::TextureParameter)
			{
				const auto Parameter = std::ranges::find(Input.Parameters, Node.GetParameterId(), &FMaterialCompilerParameterDeclaration::Id);
				if (!std::ranges::contains(Result.ActiveParameters, Parameter->Id, &FMaterialCompilerParameterDeclaration::Id))
					Result.ActiveParameters.push_back(*Parameter);
			}
		for (const auto& Node : IR.Nodes)
			if (Node.Opcode == EMaterialProgramOpcode::CollectionParameter)
			{
				const auto Reference = Node.GetCollectionParameter();
				const auto Collection = std::ranges::find(Input.Collections,
					Reference.CollectionId, &FMaterialParameterCollectionLayout::CollectionId);
				if (!Reference.CollectionId.IsValid() || !Reference.ParameterId.IsValid()
					|| Collection == Input.Collections.end())
				{
					Result.Diagnostics.push_back(MakeNormalizationFailure(
						EMaterialIRError::ParameterMissingIncompatibleBindingType));
					return Result;
				}
				const auto Field = std::ranges::find(Collection->UniformLayout.Fields,
					Reference.ParameterId, &FMaterialRenderField::ParameterId);
				const auto Expected = static_cast<EMaterialRenderValueType>(
					Node.ResultType == EMaterialProgramValueType::Float ? EMaterialRenderValueType::Scalar
					: Node.ResultType == EMaterialProgramValueType::Float2 ? EMaterialRenderValueType::Vector2
					: Node.ResultType == EMaterialProgramValueType::Float3 ? EMaterialRenderValueType::Vector3
					: EMaterialRenderValueType::Vector4);
				if (Node.ResultType > EMaterialProgramValueType::Float4
					|| Field == Collection->UniformLayout.Fields.end()
					|| Field->Type != Expected)
				{
					Result.Diagnostics.push_back(MakeNormalizationFailure(
						EMaterialIRError::ParameterMissingIncompatibleBindingType));
					return Result;
				}
				if (!std::ranges::contains(Result.ActiveCollections,
					Reference.CollectionId, &FMaterialParameterCollectionLayout::CollectionId))
					Result.ActiveCollections.push_back(*Collection);
			}
		std::ranges::sort(Result.ActiveParameters, {}, &FMaterialCompilerParameterDeclaration::Id);
		std::ranges::sort(Result.ActiveCollections, {}, &FMaterialParameterCollectionLayout::CollectionId);
		auto Layout = CompileMaterialLayout(Result.ActiveParameters, Input.Environment.ResourceLimits);
		if (!Layout)
		{
			Result.Diagnostics.push_back(MakeNormalizationFailure(FMaterialError(Layout.Validation)));
			return Result;
		}
		const auto Error = MIR::EncodeCanonical(IR, Result.CanonicalBytes);
		if (!Error)
		{
			Result.Diagnostics.push_back(MakeNormalizationFailure(std::move(Error.Error)));
			return Result;
		}
		Result.Layout = std::move(Layout.Layout);
		auto IdentityInput = Input;
		IdentityInput.Collections = Result.ActiveCollections;
		Result.Identity = BuildMaterialProgramIdentity(
			IdentityInput, Result.CanonicalBytes, Result.Layout);
		Result.IR = std::move(IR);
		Result.bSucceeded = Result.Identity.IsValid();
		if (!Result.bSucceeded) Result.Diagnostics.push_back(MakeNormalizationFailure(EMaterialIRError::IdentityUnexpectedlyResolvedZero));
		return Result;
	}

	auto MIR::EncodeCanonical(
		const MIR::FModule& IR,
		FByteBuffer& OutBytes) -> FMaterialOperationResult
	{
		OutBytes.clear();
		if (IR.Version != MIR::CurrentVersion
			|| IR.Nodes.size() > MaterialFunctionMaxExpandedNodes)
		{
			return {EMaterialIRError::InvalidStructure};
		}
		constexpr std::string_view Domain = "DurinMaterialProgramIR";
		OutBytes.insert(OutBytes.end(),
			reinterpret_cast<const std::byte*>(Domain.data()),
			reinterpret_cast<const std::byte*>(Domain.data() + Domain.size()));
		OutBytes.push_back(std::byte{0});
		AppendLittleEndian(OutBytes, IR.Version);
		AppendLittleEndian(OutBytes, static_cast<uint32>(IR.Nodes.size()));
		for (const MIR::FNode& Node : IR.Nodes)
		{
			if (!Node.HasValidPayload()
				|| !IsValidMaterialValueSemantics(Node.GetSemantics()))
			{
				OutBytes.clear();
				return {EMaterialIRError::OpcodePayloadMismatch};
			}
			AppendIRNode(OutBytes, Node);
		}
		AppendLittleEndian(OutBytes, static_cast<uint8>(IR.SurfaceRoot.bAggregate));
		AppendLittleEndian(OutBytes, IR.SurfaceRoot.AggregateExpressionIndex);
		if (IR.SurfaceRoot.bAggregate
			&& (IR.SurfaceRoot.AggregateExpressionIndex >= IR.Nodes.size()
				|| IR.Nodes[IR.SurfaceRoot.AggregateExpressionIndex].ResultType
					!= EMaterialProgramValueType::Surface
				|| IR.Nodes[IR.SurfaceRoot.AggregateExpressionIndex].SpatialKind
					!= EMaterialSpatialKind::None
				|| !MaterialStagesContain(
					IR.Nodes[IR.SurfaceRoot.AggregateExpressionIndex].LegalStages,
					EMaterialEvaluationStage::Pixel)))
		{
			OutBytes.clear();
			return {EMaterialIRError::AggregateSurfaceRootExpressionInvalid};
		}
		for (size_t Index = 0; Index < IR.SurfaceRoot.Inputs.size(); ++Index)
		{
			const auto& Input = IR.SurfaceRoot.Inputs[Index];
			AppendLittleEndian(OutBytes, static_cast<uint8>(Input.bExpression));
			AppendLittleEndian(OutBytes, Input.ExpressionIndex);
			AppendLittleEndian(OutBytes, Input.Type);
			AppendLittleEndian(OutBytes, Input.LegalStages);
			AppendLittleEndian(OutBytes, Input.SpatialKind);
			AppendLittleEndian(OutBytes, Input.CoordinateSpace);
			AppendLiteral(OutBytes, Input.Literal);
			if ((!IR.SurfaceRoot.bAggregate && Input.bExpression
					&& (Input.ExpressionIndex >= IR.Nodes.size()
						|| IR.Nodes[Input.ExpressionIndex].ResultType != Input.Type))
				|| Input.Type != GetMaterialSurfaceOutputType(GSurfaceOutputOrder[Index])
				|| Input.GetSemantics() != GetMaterialSurfaceOutputSemantics(
					GSurfaceOutputOrder[Index]))
			{
				OutBytes.clear();
				return {EMaterialIRError::PerPropertySurfaceRootInputInvalid};
			}
		}
		const auto& Offset = IR.WorldPositionOffset;
		if (Offset.Type != EMaterialProgramValueType::Float3
			|| Offset.LegalStages != EMaterialEvaluationStage::Vertex
			|| Offset.SpatialKind != EMaterialSpatialKind::None
			|| Offset.CoordinateSpace != EMaterialCoordinateSpace::None
			|| (Offset.bExpression && (Offset.ExpressionIndex >= IR.Nodes.size()
				|| !IsMaterialWorldPositionOffsetSemantics(IR.Nodes[Offset.ExpressionIndex].GetSemantics())))
			|| !std::isfinite(Offset.Literal.X) || !std::isfinite(Offset.Literal.Y) || !std::isfinite(Offset.Literal.Z))
		{
			OutBytes.clear();
			return {EMaterialIRError::PerPropertySurfaceRootInputInvalid};
		}
		AppendLittleEndian(OutBytes, static_cast<uint8>(Offset.bExpression));
		AppendLittleEndian(OutBytes, Offset.ExpressionIndex);
		AppendLiteral(OutBytes, Offset.Literal);
		if (OutBytes.size() > MaterialProgramMaxCanonicalBytes)
		{
			OutBytes.clear();
			return {EMaterialIRError::CanonicalBytesExceedSupportedBound};
		}
		return {};
	}

	static auto BuildInputIdentity(
		const FMaterialStaticProperties& StaticProperties, const FMaterialCompilerEnvironment& Environment,
		std::span<const FMaterialParameterCollectionLayout> ActiveCollections,
		FByteView CanonicalIR, const FMaterialRenderLayout& Layout)
		-> FMaterialProgramIdentity
	{
		FByteBuffer Bytes;
		Bytes.reserve(CanonicalIR.size() + 512);
		AppendString(Bytes, "DurinMaterialProgramIdentity");
		AppendBytes(Bytes, CanonicalIR);

		std::vector<FMaterialCompilerDependency> Dependencies =
			Environment.Dependencies;
		std::ranges::sort(Dependencies, [](const auto& A, const auto& B) {
			return std::tie(A.VirtualPath, A.ContentHash.HashHigh,
				A.ContentHash.HashLow)
				< std::tie(B.VirtualPath, B.ContentHash.HashHigh,
					B.ContentHash.HashLow);
		});
		AppendLittleEndian(Bytes, static_cast<uint32>(Dependencies.size()));
		for (const FMaterialCompilerDependency& Dependency : Dependencies)
		{
			AppendString(Bytes, Dependency.VirtualPath);
			AppendLittleEndian(Bytes, Dependency.ContentHash.HashLow);
			AppendLittleEndian(Bytes, Dependency.ContentHash.HashHigh);
		}

		const auto ShaderProperties = CanonicalizeMaterialShaderProperties(StaticProperties);
		AppendLittleEndian(Bytes, ShaderProperties.GeometryUsage);
		AppendLittleEndian(Bytes, ShaderProperties.PassUsage);
		AppendLittleEndian(Bytes, uint8(Environment.bIncludeEditorPasses));
		AppendLittleEndian(Bytes, ShaderProperties.BlendMode);
		AppendLittleEndian(Bytes, ShaderProperties.ShadingModel);
		AppendLittleEndian(Bytes, CanonicalFloatBits(
			ShaderProperties.OpacityMaskThreshold));
		AppendLittleEndian(Bytes, MaterialProgramIdentitySchemaVersion);
		AppendLittleEndian(Bytes, MIR::CurrentVersion);
		AppendLittleEndian(Bytes, Environment.BuilderVersion);
		AppendLittleEndian(Bytes, Environment.AdapterVersion);
		AppendString(Bytes, Environment.CompilerIdentity);
		AppendString(Bytes, Environment.Target);
		AppendLittleEndian(Bytes, Layout.Identity.Version);
		AppendGuid(Bytes, Layout.Identity.Id);
		std::vector<const FMaterialParameterCollectionLayout*> Collections;
		for (const auto& Collection : ActiveCollections) Collections.push_back(&Collection);
		std::ranges::sort(Collections, {}, [](const auto* Collection) { return Collection->CollectionId; });
		AppendLittleEndian(Bytes, static_cast<uint32>(Collections.size()));
		for (const auto* Collection : Collections)
		{
			AppendGuid(Bytes, Collection->CollectionId);
			AppendLittleEndian(Bytes, Collection->SchemaVersion);
			AppendLittleEndian(Bytes, Collection->UniformLayout.Identity.Version);
			AppendGuid(Bytes, Collection->UniformLayout.Identity.Id);
		}
		AppendLittleEndian(Bytes, Environment.ResourceLimits.SampledImages);
		AppendLittleEndian(Bytes, Environment.ResourceLimits.Samplers);
		AppendLittleEndian(Bytes, Environment.ResourceLimits.UniformBuffers);
		AppendLittleEndian(Bytes, Environment.ResourceLimits.StageResources);
		AppendLittleEndian(Bytes, Environment.ResourceLimits.UniformBufferBytes);
		AppendLittleEndian(Bytes, Environment.PassContractVersion);
		// Static selection is already represented by the selected normalized MIR.
		// Do not salt the program identity with the request configuration itself:
		// distinct configurations that select identical code share one retained program.
		constexpr auto EntryPoints = MaterialCompiledEntryPoints;
		AppendLittleEndian(Bytes, static_cast<uint32>(EntryPoints.size()));
		for (size_t Index = 0; Index < EntryPoints.size(); ++Index)
		{
			AppendString(Bytes, EntryPoints[Index]);
			AppendLittleEndian(Bytes, static_cast<uint8>(MaterialShaderEntries[Index].Frequency));
		}
		return {.Digest = FXxHash128::HashBuffer(Bytes)};
	}

	auto BuildMaterialProgramIdentity(const FMaterialStaticProperties& StaticProperties,
		const FMaterialCompilerEnvironment& Environment, std::span<const FMaterialParameterCollectionLayout> Collections,
		FByteView CanonicalIR, const FMaterialRenderLayout& Layout) -> FMaterialProgramIdentity
	{ return BuildInputIdentity(StaticProperties, Environment, Collections, CanonicalIR, Layout); }

	auto BuildMaterialProgramIdentity(const MIR::FCompilerInput& Input,
		FByteView CanonicalIR, const FMaterialRenderLayout& Layout) -> FMaterialProgramIdentity
	{
		return BuildInputIdentity(Input.StaticProperties, Input.Environment, Input.Collections, CanonicalIR, Layout);
	}

}
