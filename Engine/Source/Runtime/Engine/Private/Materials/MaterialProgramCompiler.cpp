#include "Materials/MaterialProgramCompiler.h"

#include "Materials/MaterialRenderTypes.h"
#include "Shader/ShaderCompilerCore.h"
#include "DynamicRHI.h"

#include <algorithm>
#include <array>
#include <bit>
#include <functional>
#include <numeric>
#include <tuple>

namespace Durin
{
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
		inline constexpr uint32 MaterialProgramIdentitySchemaVersion = 5;

		auto IsCommutative(EMaterialProgramOpcode Opcode) -> bool
		{
			return Opcode == EMaterialProgramOpcode::Add
				|| Opcode == EMaterialProgramOpcode::Multiply
				|| Opcode == EMaterialProgramOpcode::Minimum
				|| Opcode == EMaterialProgramOpcode::Maximum;
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
			const FMaterialIRNode& Node) -> void
		{
			AppendLittleEndian(Bytes, Node.Opcode);
			AppendLittleEndian(Bytes, Node.ResultType);
			AppendLittleEndian(Bytes, static_cast<uint32>(Node.Inputs.size()));
			for (uint32 Input : Node.Inputs) AppendLittleEndian(Bytes, Input);
			AppendLiteral(Bytes, Node.GetLiteral());
			AppendGuid(Bytes, Node.GetParameterId());
			AppendLittleEndian(Bytes, Node.GetSwizzle().Length);
			AppendLittleEndian(Bytes, Node.GetSwizzle().Components[0]);
			AppendLittleEndian(Bytes, Node.GetSwizzle().Components[1]);
			AppendLittleEndian(Bytes, Node.GetSwizzle().Components[2]);
			AppendLittleEndian(Bytes, Node.GetSwizzle().Components[3]);
		}

		auto MakeNormalizationFailure(std::string Message)
			-> FMaterialProgramDiagnostic
		{
			if (Message.size() > MaterialProgramMaxDiagnosticMessageBytes)
				Message.resize(MaterialProgramMaxDiagnosticMessageBytes);
			return {
				.Category = EMaterialProgramDiagnosticCategory::Normalization,
				.LocationKind =
					EMaterialProgramDiagnosticLocationKind::Program,
				.Message = std::move(Message)};
		}
	}

	auto BuildDefaultMaterialCompilerEnvironment(
		FMaterialCompilerEnvironment& OutEnvironment,
		std::string& OutError) -> bool
	{
		FShaderCompileOptions Options;
		Options.EntryPoints = {
			"FragmentMain", "GeometryFragmentMain",
			"OpaqueShadowFragmentMain", "ShadowFragmentMain"};
		Options.Frequencies.assign(
			Options.EntryPoints.size(), EShaderFrequency::Fragment);
		Options.Macros.emplace_back("DURIN_MATERIAL_BLEND_MODE", "1");
		Options.Macros.emplace_back("DURIN_MATERIAL_SHADING_MODEL", "1");
		Options.Macros.emplace_back(
			"DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS", "1056964608");
		FShaderSourceDependencyFingerprint SourceTree;
		if (!BuildShaderSourceTreeFingerprint(
			"/Engine/MaterialCompilerEnvironment", Options, SourceTree,
			OutError))
			return false;

		FMaterialCompilerEnvironment Environment;
		if (const auto* Capabilities = GDynamicRHI ? GDynamicRHI->RHIGetCapabilities() : nullptr)
		{
			auto& Limits = Environment.ResourceLimits;
			Limits.SampledImages = std::min(Limits.SampledImages, Capabilities->MaxFragmentSampledImages);
			Limits.Samplers = std::min(Limits.Samplers, Capabilities->MaxFragmentSamplers);
			Limits.UniformBuffers = std::min(Limits.UniformBuffers, Capabilities->MaxFragmentUniformBuffers);
			Limits.StageResources = std::min(Limits.StageResources, Capabilities->MaxFragmentResources);
			Limits.UniformBufferBytes = std::min(Limits.UniformBufferBytes, Capabilities->MaxUniformBufferRange);
		}
		Environment.CompilerIdentity =
			GetShaderCompilerEnvironmentIdentity();
		Environment.Dependencies.push_back({
			.VirtualPath = std::move(SourceTree.VirtualPath),
			.ContentHash = SourceTree.ContentHash});
		OutEnvironment = std::move(Environment);
		return true;
	}

	static auto ValidateNormalizationEnvironment(const FMaterialIRCompilerInput& Input) -> FMaterialNormalizationResult
	{
		FMaterialNormalizationResult Result;
		std::string StaticPropertiesError;
		if (!ValidateMaterialStaticProperties(
			Input.StaticProperties, StaticPropertiesError))
		{
			Result.Diagnostics.push_back(MakeNormalizationFailure(
				std::move(StaticPropertiesError)));
			return Result;
		}
		if (Input.Environment.CompilerIdentity.empty()
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

	auto NormalizeMaterialIR(const FMaterialIRCompilerInput& Input) -> FMaterialNormalizationResult
	{
		FMaterialNormalizationResult Result;
		const auto EnvironmentValidation = ValidateNormalizationEnvironment(Input);
		if (!EnvironmentValidation) return EnvironmentValidation;
		auto Validation = ValidateMaterialIR(Input.IR, Input.Parameters);
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
		std::vector<int8> Comparisons(Count * Count, 2);
		std::function<int8(uint32, uint32)> Compare = [&](uint32 A, uint32 B) -> int8 {
			if (A == B) return 0;
			auto& Cached = Comparisons[A * Count + B];
			if (Cached != 2) return Cached;
			const auto& Left = Keys[A];
			const auto& Right = Keys[B];
			int8 Order = Left.Header < Right.Header ? -1 : Left.Header > Right.Header ? 1 : 0;
			for (size_t Index = 0; Order == 0 && Index < std::min(Left.Inputs.size(), Right.Inputs.size()); ++Index)
				Order = Compare(Left.Inputs[Index], Right.Inputs[Index]);
			if (Order == 0) Order = Left.Inputs.size() < Right.Inputs.size() ? -1 : Left.Inputs.size() > Right.Inputs.size() ? 1 : 0;
			Cached = Order;
			Comparisons[B * Count + A] = -Order;
			return Order;
		};
		// Validation guarantees topological order, so all child keys already exist.
		for (size_t Index = 0; Index < Count; ++Index)
		{
			auto Header = Nodes[Index];
			Header.Inputs.clear();
			AppendIRNode(Keys[Index].Header, Header);
			Keys[Index].Inputs = Nodes[Index].Inputs;
			if (IsCommutative(Nodes[Index].Opcode))
				std::ranges::stable_sort(Keys[Index].Inputs, [&](uint32 A, uint32 B) { return Compare(A, B) < 0; });
		}
		constexpr uint32 InvalidIndex = 0xffffffffu;
		std::vector<uint32> Indices(Count, InvalidIndex);
		FMaterialIR IR;
		IR.SurfaceRoot = Input.IR.SurfaceRoot;
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
			IR.SurfaceRoot.AggregateExpressionIndex = Emit(IR.SurfaceRoot.AggregateExpressionIndex);
			for (auto& Root : IR.SurfaceRoot.Inputs) Root = {.Type = Root.Type};
		}
		else
		{
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
		for (const auto& Source : Input.Sources)
			if (Indices[Source.ExpressionIndex] != InvalidIndex)
			{
				auto Remapped = Source;
				Remapped.ExpressionIndex = Indices[Source.ExpressionIndex];
				Result.Sources.push_back(std::move(Remapped));
			}
		std::ranges::stable_sort(Result.Sources, {}, &FMaterialExpressionSource::ExpressionIndex);
		for (const auto& Node : IR.Nodes)
			if (Node.Opcode == EMaterialProgramOpcode::Parameter || Node.Opcode == EMaterialProgramOpcode::TextureParameter)
			{
				const auto Parameter = std::ranges::find(Input.Parameters, Node.GetParameterId(), &FMaterialCompilerParameterDeclaration::Id);
				if (std::ranges::find(Result.ActiveParameters, Parameter->Id, &FMaterialCompilerParameterDeclaration::Id) == Result.ActiveParameters.end())
					Result.ActiveParameters.push_back(*Parameter);
			}
		std::ranges::sort(Result.ActiveParameters, {}, &FMaterialCompilerParameterDeclaration::Id);
		auto Layout = CompileMaterialLayout(Result.ActiveParameters, Input.Environment.ResourceLimits);
		if (!Layout)
		{
			Result.Diagnostics.push_back(MakeNormalizationFailure(std::string(GetMaterialLayoutErrorText(Layout.Validation.Error))));
			return Result;
		}
		std::string Error;
		if (!EncodeMaterialIRCanonical(IR, Result.CanonicalBytes, Error))
		{
			Result.Diagnostics.push_back(MakeNormalizationFailure(std::move(Error)));
			return Result;
		}
		Result.Layout = std::move(Layout.Layout);
		Result.Identity = BuildMaterialProgramIdentity(Input, Result.CanonicalBytes, Result.Layout);
		Result.IR = std::move(IR);
		Result.bSucceeded = Result.Identity.IsValid();
		if (!Result.bSucceeded) Result.Diagnostics.push_back(MakeNormalizationFailure("Material IR identity unexpectedly resolved to zero."));
		return Result;
	}

	auto EncodeMaterialIRCanonical(
		const FMaterialIR& IR,
		FByteBuffer& OutBytes,
		std::string& OutError) -> bool
	{
		OutBytes.clear();
		OutError.clear();
		if (IR.Version != CurrentMaterialIRVersion
			|| IR.Nodes.size() > MaterialFunctionMaxExpandedNodes)
		{
			OutError = "Material IR version, node count, or surface output count is invalid.";
			return false;
		}
		constexpr std::string_view Domain = "DurinMaterialProgramIR";
		OutBytes.insert(OutBytes.end(),
			reinterpret_cast<const std::byte*>(Domain.data()),
			reinterpret_cast<const std::byte*>(Domain.data() + Domain.size()));
		OutBytes.push_back(std::byte{0});
		AppendLittleEndian(OutBytes, IR.Version);
		AppendLittleEndian(OutBytes, static_cast<uint32>(IR.Nodes.size()));
		for (const FMaterialIRNode& Node : IR.Nodes)
		{
			if (!Node.HasValidPayload())
			{
				OutBytes.clear();
				OutError = "Material IR opcode and immediate payload do not agree.";
				return false;
			}
			AppendIRNode(OutBytes, Node);
		}
		AppendLittleEndian(OutBytes, static_cast<uint8>(IR.SurfaceRoot.bAggregate));
		AppendLittleEndian(OutBytes, IR.SurfaceRoot.AggregateExpressionIndex);
		if (IR.SurfaceRoot.bAggregate
			&& (IR.SurfaceRoot.AggregateExpressionIndex >= IR.Nodes.size()
				|| IR.Nodes[IR.SurfaceRoot.AggregateExpressionIndex].ResultType
					!= EMaterialProgramValueType::Surface))
		{
			OutBytes.clear();
			OutError = "Material IR aggregate Surface Root expression is invalid.";
			return false;
		}
		for (size_t Index = 0; Index < IR.SurfaceRoot.Inputs.size(); ++Index)
		{
			const auto& Input = IR.SurfaceRoot.Inputs[Index];
			AppendLittleEndian(OutBytes, static_cast<uint8>(Input.bExpression));
			AppendLittleEndian(OutBytes, Input.ExpressionIndex);
			AppendLittleEndian(OutBytes, Input.Type);
			AppendLiteral(OutBytes, Input.Literal);
			if ((!IR.SurfaceRoot.bAggregate && Input.bExpression
					&& (Input.ExpressionIndex >= IR.Nodes.size()
						|| IR.Nodes[Input.ExpressionIndex].ResultType != Input.Type))
				|| Input.Type != GetMaterialSurfaceOutputType(GSurfaceOutputOrder[Index]))
			{
				OutBytes.clear();
				OutError = "Material IR per-property Surface Root input is invalid.";
				return false;
			}
		}
		if (OutBytes.size() > MaterialProgramMaxCanonicalBytes)
		{
			OutBytes.clear();
			OutError = "Material IR canonical bytes exceed the version-2 bound.";
			return false;
		}
		return true;
	}

	static auto BuildInputIdentity(
		const FMaterialIRCompilerInput& Input,
		FByteView CanonicalIR, const FMaterialRenderLayout& Layout)
		-> FMaterialProgramIdentity
	{
		FByteBuffer Bytes;
		Bytes.reserve(CanonicalIR.size() + 512);
		AppendString(Bytes, "DurinMaterialProgramIdentity");
		AppendBytes(Bytes, CanonicalIR);

		std::vector<FMaterialCompilerDependency> Dependencies =
			Input.Environment.Dependencies;
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

		const auto ShaderProperties = CanonicalizeMaterialShaderProperties(Input.StaticProperties);
		AppendLittleEndian(Bytes, ShaderProperties.BlendMode);
		AppendLittleEndian(Bytes, ShaderProperties.ShadingModel);
		AppendLittleEndian(Bytes, CanonicalFloatBits(
			ShaderProperties.OpacityMaskThreshold));
		AppendLittleEndian(Bytes, MaterialProgramIdentitySchemaVersion);
		AppendLittleEndian(Bytes, CurrentMaterialIRVersion);
		AppendLittleEndian(Bytes, CurrentMaterialGeneratorVersion);
		AppendLittleEndian(Bytes, CurrentMaterialCompilerEnvelopeVersion);
		AppendString(Bytes, Input.Environment.CompilerIdentity);
		AppendString(Bytes, Input.Environment.Target);
		AppendLittleEndian(Bytes, Layout.Identity.Version);
		AppendGuid(Bytes, Layout.Identity.Id);
		AppendLittleEndian(Bytes, Input.Environment.ResourceLimits.SampledImages);
		AppendLittleEndian(Bytes, Input.Environment.ResourceLimits.Samplers);
		AppendLittleEndian(Bytes, Input.Environment.ResourceLimits.UniformBuffers);
		AppendLittleEndian(Bytes, Input.Environment.ResourceLimits.StageResources);
		AppendLittleEndian(Bytes, Input.Environment.ResourceLimits.UniformBufferBytes);
		AppendLittleEndian(Bytes, Input.Environment.PassContractVersion);
		constexpr std::array<std::string_view, 3> EntryPoints{
			"FragmentMain", "GeometryFragmentMain", "ShadowFragmentMain"};
		AppendLittleEndian(Bytes, static_cast<uint32>(EntryPoints.size()));
		for (std::string_view EntryPoint : EntryPoints)
		{
			AppendString(Bytes, EntryPoint);
			AppendLittleEndian(Bytes, static_cast<uint8>(1));
		}
		return {.Digest = FXxHash128::HashBuffer(Bytes)};
	}

	auto BuildMaterialProgramIdentity(const FMaterialIRCompilerInput& Input,
		FByteView CanonicalIR, const FMaterialRenderLayout& Layout) -> FMaterialProgramIdentity
	{
		return BuildInputIdentity(Input, CanonicalIR, Layout);
	}

}
