#include "VertexFactory.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialBindingSchema.h"
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
			AppendLittleEndian(Bytes, Node.GetTextureReadMode());
			if (const auto* Read = std::get_if<MIR::FSceneTextureRead>(&Node.Payload))
			{ AppendLittleEndian(Bytes, Read->SceneTextureId); AppendLittleEndian(Bytes, Read->Filter); }
			if (const auto* Read = std::get_if<MIR::FCustomDataRead>(&Node.Payload))
				AppendLittleEndian(Bytes, Read->Index);
		}

		auto MakeNormalizationFailure(std::string Message)
			-> FMaterialProgramDiagnostic
		{
			return {
				.LocationKind =
					EMaterialProgramDiagnosticLocationKind::Program,
				.Message = std::move(Message)};
		}
	}

	auto MakeMaterialShaderCompileOptions(FShaderTargetIdentity Target, const FMaterialStaticProperties& Properties) -> FShaderCompileOptions
	{
		FShaderCompileOptions Options; Options.Target = Target;
		auto OrderedEntries = GetDefaultMaterialShaderEntries();
		std::ranges::sort(OrderedEntries);
		for (const auto& Name : OrderedEntries)
		{
			const auto& Entry = *FindMaterialShaderEntry(Name);
			if (!std::ranges::contains(Options.EntryPoints, std::string(Entry.Name))) {
				Options.EntryPoints.emplace_back(Entry.Name.data()); Options.Frequencies.push_back(Entry.Frequency);
			}
		}
		Options.Macros.emplace_back("DURIN_MATERIAL_BLEND_MODE", std::to_string(static_cast<uint8>(Properties.BlendMode)));
		Options.Macros.emplace_back("DURIN_MATERIAL_SHADING_MODEL", std::to_string(static_cast<uint8>(Properties.ShadingModel)));
		Options.Macros.emplace_back("DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS", std::to_string(std::bit_cast<uint32>(Properties.OpacityMaskThreshold)));
		return Options;
	}

	auto GetMaterialVertexFactorySourceRoots() -> std::vector<std::string>
	{
		std::set<std::string> ExtraRoots;
		for (const auto& D : GetMaterialShaderEntries()) if (D.VertexFactory) {
			const auto& VF = D.VertexFactory()->GetShaderDescription();
			for (const auto ModuleName : VF.ShaderModules) {
				std::string Module(ModuleName); std::ranges::replace(Module, '.', '/');
				ExtraRoots.insert("/Engine/" + Module);
			}
			std::string Include(VF.MaterialEntryInclude);
			if (Include.ends_with(".slang")) Include.resize(Include.size() - 6);
			ExtraRoots.insert("/Engine/" + Include);
		}
		return {ExtraRoots.begin(), ExtraRoots.end()};
	}

	auto BuildDefaultMaterialCompilerEnvironment(
		FMaterialCompilerEnvironment& OutEnvironment,
		std::optional<FShaderTargetIdentity> Target) -> std::expected<void, std::string>
	{
		FShaderCompileOptions Options;
		const auto SelectedTarget = Target.value_or(Options.Target);
#if DURIN_WITH_EDITOR
		const auto Platform = ToCookTargetPlatform(SelectedTarget.Platform);
		const auto Resolved = Platform ? ResolveTargetPlatform(*Platform)
			: nullptr;
		if (!Resolved || (Resolved->GetShaderTarget() != SelectedTarget && Resolved->GetCookedShaderTarget() != SelectedTarget)) return std::unexpected("Material shader target is unsupported.");
#endif
		const bool bMetal = IsMetalShaderTarget(SelectedTarget);
		Options = MakeMaterialShaderCompileOptions(SelectedTarget, {.OpacityMaskThreshold = 0.5f});
		FShaderSourceDependencyFingerprint SourceTree;
		FShaderOperationResult ProviderError;
		if (!(ProviderError = BuildShaderSourceTreeFingerprint("/Engine/MaterialCompilerEnvironment", Options, SourceTree)))
		{
			return std::unexpected(std::string(ProviderError.error()));
		}

		FMaterialCompilerEnvironment Environment;
		Environment.Target = SelectedTarget == MetalLibraryShaderTarget ? "metal-library-1"
			: bMetal ? "metal-msl-2.0" : "vulkan-spirv-1.5";
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
		auto* ShaderBuild = IShaderBuildModule::Get();
		const uint64 ShaderBuildVersion = ShaderBuild ? ShaderBuild->GetBuildVersion() : 0;
		if (!ShaderBuildVersion)
			return std::unexpected("Shader build identity is unavailable.");
		const auto Adapter = ShaderBuild->GetCompileAdapterVersion();
		Environment.CompilerIdentity = std::format("ShaderBuild:{:016x};adapter={}:{}:{}",
			ShaderBuildVersion, Adapter.FunctionVersion, Adapter.ConstantsSchema, Adapter.OutputSchema);
		std::vector<std::pair<std::string, FXxHash128>> ParameterOwners;
		for (const auto& Entry : GetMaterialShaderEntries()) {
			const auto Schema = BuildMaterialParameterSchema(Entry.ShaderType, {}, {});
			if (!Schema) return std::unexpected(Schema.error());
			ParameterOwners.emplace_back(Entry.ShaderType, Schema->Identity);
		}
		std::ranges::sort(ParameterOwners, {}, &std::pair<std::string, FXxHash128>::first);
		FXxHash128Builder ParameterIdentity;
		ParameterIdentity.UpdateValue(static_cast<uint32>(ParameterOwners.size()));
		for (const auto& [Name, Identity] : ParameterOwners) {
			ParameterIdentity.UpdateValue(static_cast<uint32>(Name.size())); ParameterIdentity.Update(Name);
			ParameterIdentity.UpdateValue(Identity.HashLow); ParameterIdentity.UpdateValue(Identity.HashHigh);
		}
		Environment.CompilerIdentity += ";parameters=" + ParameterIdentity.Finalize().ToString();
		if (SelectedTarget == MetalLibraryShaderTarget)
		{
			const auto Producer = ShaderBuild->GetTargetProducerIdentity(SelectedTarget);
			if (!Producer) return std::unexpected(Producer.error());
			Environment.CompilerIdentity += ";native=" + *Producer;
		}
		Environment.Dependencies.push_back({
			.VirtualPath = std::move(SourceTree.VirtualPath),
			.ContentHash = SourceTree.ContentHash});
		std::vector<FShaderSourceDependencyFingerprint> BaseClosure;
		if (auto R = BuildShaderSourceDependencyManifest("/Engine/MaterialCompilerEnvironment", Options, BaseClosure); !R)
			return std::unexpected(R.error());
		const auto ExtraRoots = GetMaterialVertexFactorySourceRoots();
		for (const auto& Root : ExtraRoots) {
			if (std::ranges::any_of(BaseClosure, [&](const auto& D) { return D.VirtualPath == Root || D.VirtualPath == Root + ".slang"; })) continue;
			FShaderSourceDependencyFingerprint Extra;
			if (auto R = BuildShaderSourceTreeFingerprint(Root, Options, Extra); !R) return std::unexpected(R.error());
			Environment.Dependencies.push_back({Extra.VirtualPath, Extra.ContentHash});
		}
		std::ranges::sort(Environment.Dependencies, {}, &FMaterialCompilerDependency::VirtualPath);
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
				StaticPropertiesError.error()));
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
				"Material compiler environment identity, target, pass contract, or dependency bounds are invalid."));
			return Result;
		}
		if (!IsSupportedMaterialQualityLevel(Input.Environment.Quality)
			|| !IsSupportedMaterialFeatureLevel(Input.Environment.FeatureLevel)
			|| Input.Environment.StaticBools.size() > MaterialMaxStaticBoolDeclarations
			|| !std::ranges::is_sorted(Input.Environment.StaticBools, {},
				&FMaterialCompilerEnvironment::FStaticBoolValue::DeclarationId))
		{
			Result.Diagnostics.push_back(MakeNormalizationFailure(
				"Material compiler environment identity, target, pass contract, or dependency bounds are invalid."));
			return Result;
		}
		for (size_t Index = 0; Index < Input.Environment.StaticBools.size(); ++Index)
			if (!Input.Environment.StaticBools[Index].DeclarationId.IsValid()
				|| (Index && Input.Environment.StaticBools[Index - 1].DeclarationId
					== Input.Environment.StaticBools[Index].DeclarationId))
			{
				Result.Diagnostics.push_back(MakeNormalizationFailure(
					"Material compiler environment identity, target, pass contract, or dependency bounds are invalid."));
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
					"Material compiler dependency manifest contains an invalid entry."));
				return Result;
			}
			for (size_t Other = 0; Other < Index; ++Other)
				if (Input.Environment.Dependencies[Other].VirtualPath
					== Dependency.VirtualPath)
				{
					Result.Diagnostics.push_back(MakeNormalizationFailure(
						"Material compiler dependency manifest contains a duplicate virtual path."));
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
		if (const auto Valid = ValidateMaterialConfiguration(Input.IR.Domain, Input.StaticProperties); !Valid)
		{ Result.Diagnostics.push_back(MakeNormalizationFailure(Valid.error())); return Result; }
		if (Input.Collections.size() > MaterialParameterCollectionMaxPerMaterial)
		{
			Result.Diagnostics.push_back(MakeNormalizationFailure(
				"IR parameter declaration count exceeds its bound."));
			return Result;
		}
		if (Input.Collections.size() + 5 > Input.Environment.ResourceLimits.UniformBuffers)
		{
			Result.Diagnostics.push_back(MakeNormalizationFailure(
				"IR parameter declaration count exceeds its bound."));
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
					"IR parameter declaration is invalid or duplicated."));
				return Result;
			}
		auto Validation = MIR::Validate(Input.IR, Input.Parameters);
		if (!Validation) { Result.Diagnostics = std::move(Validation.Diagnostics); return Result; }
		if (Input.Sources.size() > MaterialFunctionMaxExpandedNodes)
		{
			Result.Diagnostics.push_back(MakeNormalizationFailure("IR source metadata count exceeds its bound."));
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
				Result.Diagnostics.push_back(MakeNormalizationFailure("IR source metadata is invalid or exceeds its bound."));
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
		IR.Domain = Input.IR.Domain;
		IR.AttributeRoots = Input.IR.AttributeRoots;
		IR.bAggregateAttributes = Input.IR.bAggregateAttributes;
		IR.AggregateAttributesIndex = Input.IR.AggregateAttributesIndex;
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
		if (IR.bAggregateAttributes)
		{
			// Surface construction is lowered before normalization. Both authoring
			// modes share final-property filtering and reachability from this point.
			const auto& Surface = Nodes[IR.AggregateAttributesIndex];
			for (size_t Index = 0; Index < IR.AttributeRoots.size(); ++Index)
			{
				auto& Root = IR.GetAttribute(FMaterialAttributeDefinitionMap::GetDefinitions()[Index].Property);
				Root.bExpression = true;
				Root.ExpressionIndex = Index < Surface.Inputs.size() ? Surface.Inputs[Index] : 0;
				Root.bExpression = Index < Surface.Inputs.size();
			}
			IR.bAggregateAttributes = false;
		}
		{
			const FMaterialAttributeValues Defaults;
			std::set<EMaterialProperty> Demanded;
			for (const auto Entry : SelectMaterialShaderEntries(Input.StaticProperties, Input.Environment.bIncludeEditorPasses, Input.IR.Domain, &Input.Environment))
				for (const auto Property : GetMaterialEntryProperties(FindMaterialShaderEntry(Entry)->Entry, Input.StaticProperties)) Demanded.insert(Property);
			for (size_t Index = 0; Index < IR.AttributeRoots.size(); ++Index)
				if (!Demanded.contains(FMaterialAttributeDefinitionMap::GetDefinitions()[Index].Property)
					|| (IR.Domain == EMaterialDomain::Surface && !IsMaterialSurfaceOutputActive(FMaterialAttributeDefinitionMap::GetDefinitions()[Index].Property, Input.StaticProperties)))
				{
					auto& Root = IR.GetAttribute(FMaterialAttributeDefinitionMap::GetDefinitions()[Index].Property);
					Root.bExpression = false;
					Root.Literal = GetMaterialSurfaceOutputDefault(Defaults, FMaterialAttributeDefinitionMap::GetDefinitions()[Index].Property);
				}

			IR.AggregateAttributesIndex = 0;
			std::ranges::sort(IR.AttributeRoots, {}, &MIR::FModule::FAttributeRoot::AttributeID);
			for (auto& Root : IR.AttributeRoots)
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
						"Material IR parameter is missing or has an incompatible binding type."));
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
						"Material IR parameter is missing or has an incompatible binding type."));
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
			Result.Diagnostics.push_back(MakeNormalizationFailure(std::format("{}{} [index {}]", GetMaterialLayoutErrorText(Layout.Validation.Error), Layout.Validation.ParameterId.IsValid() ? std::format(" [parameter {}]", Layout.Validation.ParameterId.ToString()) : "", Layout.Validation.FieldIndex)));
			return Result;
		}
		if (const auto Domain = ValidateMaterialEvaluationDomain(IR); !Domain)
		{ Result.Diagnostics.push_back(MakeNormalizationFailure(Domain.error())); return Result; }
		const auto Error = MIR::EncodeCanonical(IR, Result.CanonicalBytes);
		if (!Error)
		{
			Result.Diagnostics.push_back(MakeNormalizationFailure(Error.error()));
			return Result;
		}
		Result.Layout = std::move(Layout.Layout);
		auto IdentityInput = Input;
		IdentityInput.Collections = Result.ActiveCollections;
		Result.Identity = BuildMaterialProgramIdentity(
			IdentityInput, Result.CanonicalBytes, Result.Layout);
		Result.IR = std::move(IR);
		Result.bSucceeded = Result.Identity.IsValid();
		if (!Result.bSucceeded) Result.Diagnostics.push_back(MakeNormalizationFailure("Material IR identity unexpectedly resolved to zero."));
		return Result;
	}

	auto MIR::EncodeCanonical(
		const MIR::FModule& IR,
		FByteBuffer& OutBytes) -> std::expected<void, std::string>
	{
		OutBytes.clear();
		if (IR.Version != MIR::CurrentVersion
			|| IR.Nodes.size() > MaterialFunctionMaxExpandedNodes)
		{
			return std::unexpected("Material IR version, node count, or surface output count is invalid.");
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
				return std::unexpected("Material IR opcode and immediate payload do not agree.");
			}
			AppendIRNode(OutBytes, Node);
		}
		if (IR.Domain > EMaterialDomain::PostProcess)
		{ OutBytes.clear(); return std::unexpected("Unsupported material domain."); }
		AppendLittleEndian(OutBytes, IR.Domain);
		AppendLittleEndian(OutBytes, static_cast<uint8>(IR.bAggregateAttributes));
		AppendLittleEndian(OutBytes, IR.AggregateAttributesIndex);
		if (IR.bAggregateAttributes
			&& (IR.AggregateAttributesIndex >= IR.Nodes.size()
				|| IR.Nodes[IR.AggregateAttributesIndex].ResultType
					!= EMaterialProgramValueType::MaterialAttributes
				|| IR.Nodes[IR.AggregateAttributesIndex].SpatialKind
					!= EMaterialSpatialKind::None
				|| !MaterialStagesContain(
					IR.Nodes[IR.AggregateAttributesIndex].LegalStages,
					EMaterialEvaluationStage::Pixel)))
		{
			OutBytes.clear();
			return std::unexpected("Material IR aggregate Surface Root expression is invalid.");
		}
		auto Roots = IR.AttributeRoots;
		if (Roots.size() != FMaterialAttributeDefinitionMap::GetDefinitions().size())
		{
			OutBytes.clear();
			return std::unexpected("Material IR attribute schema is incomplete.");
		}
		std::ranges::sort(Roots, {}, &MIR::FModule::FAttributeRoot::AttributeID);
		std::unordered_set<FGuid> Seen;
		AppendLittleEndian(OutBytes, uint32(Roots.size()));
		for (const auto& Input : Roots)
		{
			const auto* Definition = FMaterialAttributeDefinitionMap::Find(Input.AttributeID);
			if (!Definition || !Seen.insert(Input.AttributeID).second || Input.GetSemantics() != Definition->Semantics
				|| !std::isfinite(Input.Literal.X) || !std::isfinite(Input.Literal.Y)
				|| !std::isfinite(Input.Literal.Z) || !std::isfinite(Input.Literal.W)
				|| (Input.bExpression && (Input.ExpressionIndex >= IR.Nodes.size() || IR.Nodes[Input.ExpressionIndex].ResultType != Input.Type)))
			{ OutBytes.clear(); return std::unexpected("Material IR attribute root is invalid."); }
			AppendGuid(OutBytes, Input.AttributeID);
			AppendLittleEndian(OutBytes, static_cast<uint8>(Input.bExpression));
			AppendLittleEndian(OutBytes, Input.ExpressionIndex);
			AppendLittleEndian(OutBytes, Input.Type);
			AppendLittleEndian(OutBytes, Input.LegalStages);
			AppendLittleEndian(OutBytes, Input.SpatialKind);
			AppendLittleEndian(OutBytes, Input.CoordinateSpace);
			AppendLiteral(OutBytes, Input.Literal);
		}

		if (OutBytes.size() > MaterialProgramMaxCanonicalBytes)
		{
			OutBytes.clear();
			return std::unexpected("Material IR canonical bytes exceed the supported byte bound.");
		}
		return {};
	}

	static auto BuildInputIdentity(
		const FMaterialStaticProperties& StaticProperties, const FMaterialCompilerEnvironment& Environment,
		std::span<const FMaterialParameterCollectionLayout> ActiveCollections,
		FByteView CanonicalIR, const FMaterialRenderLayout& Layout, EMaterialDomain Domain = EMaterialDomain::Surface)
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
		const auto Selected = SelectMaterialShaderEntries(StaticProperties, Environment.bIncludeEditorPasses, Domain, &Environment);
		AppendLittleEndian(Bytes, static_cast<uint32>(Selected.size()));
		for (const auto& Name : Selected)
		{
			const auto& D = *FindMaterialShaderEntry(Name);
			AppendString(Bytes, D.ShaderType); AppendString(Bytes, D.Name);
			AppendLittleEndian(Bytes, static_cast<uint8>(D.Frequency)); AppendLittleEndian(Bytes, D.PermutationId);
			const auto Options = GetMaterialShaderCompilationOptions(Name, ShaderProperties, Domain, Environment);
			AppendLittleEndian(Bytes, uint32(Options.Macros.size()));
			for (const auto& M : Options.Macros) { AppendString(Bytes, M.Name); AppendLittleEndian(Bytes, uint8(M.Value.has_value())); if (M.Value) AppendString(Bytes, *M.Value); }
			if (D.VertexFactory) {
				const auto* VF = D.VertexFactory(); AppendString(Bytes, VF->GetName());
				for (const auto Module : VF->GetShaderDescription().ShaderModules) AppendString(Bytes, Module); AppendString(Bytes, VF->GetShaderDescription().MaterialEntryInclude);
			}
		}
		return {.Digest = FXxHash128::HashBuffer(Bytes)};
	}

	auto BuildMaterialProgramIdentity(const FMaterialStaticProperties& StaticProperties,
		const FMaterialCompilerEnvironment& Environment, std::span<const FMaterialParameterCollectionLayout> Collections,
		FByteView CanonicalIR, const FMaterialRenderLayout& Layout, EMaterialDomain Domain) -> FMaterialProgramIdentity
	{ return BuildInputIdentity(StaticProperties, Environment, Collections, CanonicalIR, Layout, Domain); }

	auto BuildMaterialProgramIdentity(const MIR::FCompilerInput& Input,
		FByteView CanonicalIR, const FMaterialRenderLayout& Layout) -> FMaterialProgramIdentity
	{
		return BuildInputIdentity(Input.StaticProperties, Input.Environment, Input.Collections, CanonicalIR, Layout, Input.IR.Domain);
	}

}
