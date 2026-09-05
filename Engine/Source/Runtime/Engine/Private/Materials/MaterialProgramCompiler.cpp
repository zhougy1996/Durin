#include "Materials/MaterialProgramCompiler.h"

#include "Materials/MaterialInterface.h"
#include "Materials/MaterialRenderTypes.h"
#include "Shader/ShaderCompilerCore.h"
#include "Threading/RunnableThread.h"

#include <algorithm>
#include <array>
#include <bit>
#include <functional>
#include <numeric>
#include <tuple>
#include <unordered_map>

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
		inline constexpr uint32 MaterialProgramIdentitySchemaVersion = 2;

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
			AppendLiteral(Bytes, Node.Literal);
			AppendGuid(Bytes, Node.ParameterId);
			AppendLittleEndian(Bytes, Node.SwizzleLength);
			AppendLittleEndian(Bytes, Node.SwizzleX);
			AppendLittleEndian(Bytes, Node.SwizzleY);
			AppendLittleEndian(Bytes, Node.SwizzleZ);
			AppendLittleEndian(Bytes, Node.SwizzleW);
		}

		auto CopyRelevantImmediates(
			const FMaterialProgramNode& Node,
			FMaterialIRNode& OutNode) -> void
		{
			switch (Node.Opcode)
			{
			case EMaterialProgramOpcode::Constant:
				OutNode.Literal.X = Node.Literal.X;
				if (Node.ResultType >= EMaterialProgramValueType::Float2)
					OutNode.Literal.Y = Node.Literal.Y;
				if (Node.ResultType >= EMaterialProgramValueType::Float3)
					OutNode.Literal.Z = Node.Literal.Z;
				if (Node.ResultType >= EMaterialProgramValueType::Float4)
					OutNode.Literal.W = Node.Literal.W;
				break;
			case EMaterialProgramOpcode::Parameter:
			case EMaterialProgramOpcode::TextureParameter:
			case EMaterialProgramOpcode::TextureCoordinate:
				OutNode.ParameterId = Node.ParameterId;
				break;
			case EMaterialProgramOpcode::Swizzle:
				OutNode.SwizzleLength = Node.SwizzleLength;
				OutNode.SwizzleX = Node.SwizzleX;
				if (Node.SwizzleLength > 1) OutNode.SwizzleY = Node.SwizzleY;
				if (Node.SwizzleLength > 2) OutNode.SwizzleZ = Node.SwizzleZ;
				if (Node.SwizzleLength > 3) OutNode.SwizzleW = Node.SwizzleW;
				break;
			default: break;
			}
		}

		auto MakeIRNode(
			const FMaterialProgramNode& Node,
			std::span<const FMaterialProgramLink> OrderedInputs,
			const std::unordered_map<FGuid, uint32>& NormalizedIndices)
			-> FMaterialIRNode
		{
			FMaterialIRNode Result;
			Result.Opcode = Node.Opcode;
			Result.ResultType = Node.ResultType;
			CopyRelevantImmediates(Node, Result);
			Result.Inputs.reserve(OrderedInputs.size());
			for (const FMaterialProgramLink& Input : OrderedInputs)
				Result.Inputs.push_back(
					NormalizedIndices.at(Input.SourceNodeId));
			return Result;
		}

		auto MakeDefinitions(
			std::span<const FMaterialCompilerParameterDeclaration> Parameters)
			-> std::vector<FMaterialParameterDefinition>
		{
			std::vector<FMaterialParameterDefinition> Definitions;
			Definitions.reserve(Parameters.size());
			for (const auto& Parameter : Parameters)
				Definitions.push_back({.Id = Parameter.Id, .Type = Parameter.Type});
			return Definitions;
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

	auto SnapshotMaterialCompilerInput(
		const DMaterialInterface& Material,
		FMaterialCompilerEnvironment Environment,
		FMaterialCompilerInput& OutInput,
		FMaterialProgramValidationResult& OutValidation) -> bool
	{
		check(IsInGameThread());
		const FMaterialProgram* Program = Material.GetMaterialProgram();
		if (Program == nullptr)
		{
			OutValidation = {};
			OutValidation.Diagnostics.push_back({
				.Category = EMaterialProgramDiagnosticCategory::Schema,
				.LocationKind =
					EMaterialProgramDiagnosticLocationKind::Program,
				.Message = "Material has no root authored program."});
			return false;
		}
		const auto Definitions = Material.GetParameterDefinitions();
		OutValidation = ValidateMaterialProgram(*Program, Definitions);
		if (!OutValidation) return false;

		FMaterialCompilerInput Snapshot;
		Snapshot.Program = *Program;
		Snapshot.StaticProperties = Material.GetStaticProperties();
		Snapshot.Environment = std::move(Environment);
		Snapshot.Parameters.reserve(Definitions.size());
		for (const FMaterialParameterDefinition& Definition : Definitions)
			Snapshot.Parameters.push_back({
				.Id = Definition.Id, .Type = Definition.Type});
		std::ranges::sort(Snapshot.Parameters, {},
			&FMaterialCompilerParameterDeclaration::Id);
		std::ranges::sort(Snapshot.Environment.Dependencies, {},
			&FMaterialCompilerDependency::VirtualPath);
		OutInput = std::move(Snapshot);
		return true;
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
		Environment.CompilerIdentity =
			GetShaderCompilerEnvironmentIdentity();
		Environment.Dependencies.push_back({
			.VirtualPath = std::move(SourceTree.VirtualPath),
			.ContentHash = SourceTree.ContentHash});
		OutEnvironment = std::move(Environment);
		return true;
	}

	auto NormalizeMaterialProgram(const FMaterialCompilerInput& Input)
		-> FMaterialNormalizationResult
	{
		FMaterialNormalizationResult Result;
		const std::vector<FMaterialParameterDefinition> Definitions =
			MakeDefinitions(Input.Parameters);
		const FMaterialProgramValidationResult Validation =
			ValidateMaterialProgram(Input.Program, Definitions);
		if (!Validation)
		{
			Result.Diagnostics = Validation.Diagnostics;
			return Result;
		}
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

		std::unordered_map<FGuid, size_t> AuthoredIndices;
		AuthoredIndices.reserve(Input.Program.Nodes.size());
		for (size_t Index = 0; Index < Input.Program.Nodes.size(); ++Index)
			AuthoredIndices.emplace(Input.Program.Nodes[Index].Id, Index);

		std::vector<bool> Reachable(Input.Program.Nodes.size(), false);
		std::function<void(size_t)> MarkReachable = [&](size_t Index) {
			if (Reachable[Index]) return;
			Reachable[Index] = true;
			for (const FMaterialProgramLink& Link
				: Input.Program.Nodes[Index].Inputs)
				MarkReachable(AuthoredIndices.at(Link.SourceNodeId));
		};
		for (EMaterialSurfaceOutput Output : GSurfaceOutputOrder)
		{
			const FMaterialProgramLink& Link = GetMaterialSurfaceOutputLink(
				Input.Program.Outputs, Output);
			if (Link.SourceNodeId.IsValid())
				MarkReachable(AuthoredIndices.at(Link.SourceNodeId));
		}
		if (Input.Program.Outputs.Surface.SourceNodeId.IsValid())
			MarkReachable(AuthoredIndices.at(
				Input.Program.Outputs.Surface.SourceNodeId));

		FMaterialIR IR;
		const size_t ReachableCount = std::ranges::count(Reachable, true);
		IR.Nodes.reserve(ReachableCount);
		std::unordered_map<FGuid, uint32> NormalizedIndices;
		NormalizedIndices.reserve(IR.Nodes.capacity());

		std::unordered_map<FGuid, FByteBuffer> StructuralKeys;
		StructuralKeys.reserve(IR.Nodes.capacity());
		std::function<const FByteBuffer&(const FGuid&)>
			BuildStructuralKey = [&](const FGuid& Id)
				-> const FByteBuffer& {
			if (const auto Existing = StructuralKeys.find(Id);
				Existing != StructuralKeys.end()) return Existing->second;
			const FMaterialProgramNode& Node =
				Input.Program.Nodes[AuthoredIndices.at(Id)];
			FMaterialIRNode Header;
			Header.Opcode = Node.Opcode;
			Header.ResultType = Node.ResultType;
			CopyRelevantImmediates(Node, Header);
			FByteBuffer Key;
			AppendIRNode(Key, Header);
			std::vector<FByteBuffer> InputKeys;
			InputKeys.reserve(Node.Inputs.size());
			for (const FMaterialProgramLink& Link : Node.Inputs)
				InputKeys.push_back(BuildStructuralKey(Link.SourceNodeId));
			if (IsCommutative(Node.Opcode)) std::ranges::sort(InputKeys);
			for (const auto& InputKey : InputKeys) AppendBytes(Key, InputKey);
			return StructuralKeys.emplace(Id, std::move(Key)).first->second;
		};

		std::function<uint32(const FGuid&)> EmitNode = [&](const FGuid& Id) {
			if (const auto Existing = NormalizedIndices.find(Id);
				Existing != NormalizedIndices.end()) return Existing->second;
			const FMaterialProgramNode& Node =
				Input.Program.Nodes[AuthoredIndices.at(Id)];
			std::vector<FMaterialProgramLink> OrderedInputs = Node.Inputs;
			if (IsCommutative(Node.Opcode))
				std::ranges::stable_sort(OrderedInputs, [&](const auto& A,
					const auto& B) {
					return std::ranges::lexicographical_compare(
						BuildStructuralKey(A.SourceNodeId),
						BuildStructuralKey(B.SourceNodeId));
				});
			for (const FMaterialProgramLink& Link : OrderedInputs)
				EmitNode(Link.SourceNodeId);
			const uint32 IRIndex = static_cast<uint32>(IR.Nodes.size());
			IR.Nodes.push_back(MakeIRNode(
				Node, OrderedInputs, NormalizedIndices));
			NormalizedIndices.emplace(Id, IRIndex);
			return IRIndex;
		};

		for (EMaterialSurfaceOutput Output : GSurfaceOutputOrder)
		{
			const FMaterialProgramLink& Link = GetMaterialSurfaceOutputLink(
				Input.Program.Outputs, Output);
			if (Link.SourceNodeId.IsValid()) EmitNode(Link.SourceNodeId);
		}
		if (Input.Program.Outputs.Surface.SourceNodeId.IsValid())
			EmitNode(Input.Program.Outputs.Surface.SourceNodeId);

		if (IR.Nodes.size() != ReachableCount)
		{
			Result.Diagnostics.push_back(MakeNormalizationFailure(
				"Material normalization did not consume the complete reachable DAG."));
			return Result;
		}
		if (Input.Program.Outputs.Surface.SourceNodeId.IsValid())
		{
			IR.SurfaceRoot.bAggregate = true;
			IR.SurfaceRoot.AggregateExpressionIndex = NormalizedIndices.at(
				Input.Program.Outputs.Surface.SourceNodeId);
		}
		for (size_t OutputIndex = 0; OutputIndex < GSurfaceOutputOrder.size(); ++OutputIndex)
		{
			const EMaterialSurfaceOutput Output = GSurfaceOutputOrder[OutputIndex];
			auto& RootInput = IR.SurfaceRoot.Inputs[OutputIndex];
			RootInput.Type = GetMaterialSurfaceOutputType(Output);
			RootInput.Literal = GetMaterialSurfaceOutputDefault(Input.Program.Outputs, Output);
			const FMaterialProgramLink& Link = GetMaterialSurfaceOutputLink(Input.Program.Outputs, Output);
			if (Link.SourceNodeId.IsValid())
			{
				RootInput.bExpression = true;
				RootInput.ExpressionIndex = NormalizedIndices.at(Link.SourceNodeId);
			}
		}

		std::string EncodeError;
		if (!EncodeMaterialIRCanonical(IR, Result.CanonicalBytes, EncodeError))
		{
			Result.Diagnostics.push_back(
				MakeNormalizationFailure(std::move(EncodeError)));
			return Result;
		}
		Result.IR = std::move(IR);
		Result.Identity = BuildMaterialProgramIdentity(
			Input, Result.CanonicalBytes);
		Result.bSucceeded = Result.Identity.IsValid();
		if (!Result.bSucceeded)
			Result.Diagnostics.push_back(MakeNormalizationFailure(
				"Material program identity unexpectedly resolved to zero."));
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
			|| IR.Nodes.size() > MaterialProgramMaxNodeCount)
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
			AppendIRNode(OutBytes, Node);
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

	auto BuildMaterialProgramIdentity(
		const FMaterialCompilerInput& Input,
		FByteView CanonicalIR)
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

		AppendLittleEndian(Bytes, Input.StaticProperties.BlendMode);
		AppendLittleEndian(Bytes, Input.StaticProperties.ShadingModel);
		AppendLittleEndian(Bytes, CanonicalFloatBits(
			Input.StaticProperties.OpacityMaskThreshold));
		AppendLittleEndian(Bytes, MaterialProgramIdentitySchemaVersion);
		AppendLittleEndian(Bytes, CurrentMaterialIRVersion);
		AppendLittleEndian(Bytes, CurrentMaterialGeneratorVersion);
		AppendLittleEndian(Bytes, CurrentMaterialCompilerEnvelopeVersion);
		AppendString(Bytes, Input.Environment.CompilerIdentity);
		AppendString(Bytes, Input.Environment.Target);
		AppendLittleEndian(Bytes, CurrentMaterialRenderLayoutVersion);
		AppendGuid(Bytes, MaterialRenderLayoutV3Id);
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
}
