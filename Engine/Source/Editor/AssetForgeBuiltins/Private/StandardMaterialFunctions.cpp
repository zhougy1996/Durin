#include "AssetForge/Builtins/StandardMaterialFunctions.h"

#include "Asset/Asset.h"
#include "Asset/PackageSerialization.h"
#include "AssetTools/IAssetTools.h"
#include "DObject/Package.h"

#include <algorithm>

namespace Durin::AssetForge::Builtins
{
	namespace
	{
		using Type = EMaterialProgramValueType;
		using Op = EMaterialProgramOpcode;
		using Kind = EMaterialFunctionDefaultKind;
		using Entry = EStandardMaterialFunction;
		using Link = FMaterialProgramLink;
		constexpr std::array RoleNames{"BaseColor", "Normal", "Metallic", "Roughness",
			"AmbientOcclusion", "Emissive", "Opacity", "OpacityMask"};
		constexpr std::array EntryNames{"UVTransform", "SampleNormal", "SampleORM", "StandardPBR", "StandardPBR_ORM", "ImportedSurfaceValues", "DecodeImportedNormalRG"};
		constexpr auto Id(Entry Function, uint32 Slot) -> FGuid { return StandardMaterialPortId(Function, Slot); }
		auto Numeric(float X, float Y = 0, float Z = 0) -> FMaterialFunctionDefault
			{ return {.Kind = Kind::Numeric, .Numeric = {.X = X, .Y = Y, .Z = Z}}; }
		auto Texture(EMaterialTextureFallback Fallback = EMaterialTextureFallback::White)
			-> FMaterialFunctionDefault { return {.Kind = Kind::Texture, .TextureFallback = Fallback}; }

		struct FBuilder
		{
			Entry Family;
			FMaterialFunctionGraph Graph;
			auto Node(Op Opcode, Type ResultType, std::vector<Link> Inputs = {}) -> Link
			{
				FMaterialProgramNode Node{.Id = {0xf67a24b1, 0x4378491a,
					static_cast<uint32>(Family), static_cast<uint32>(Graph.Nodes.size() + 1)},
					.Opcode = Opcode, .ResultType = ResultType, .Inputs = std::move(Inputs)};
				Graph.Nodes.push_back(std::move(Node));
				return {.SourceNodeId = Graph.Nodes.back().Id};
			}
			auto Input(uint32 Slot, std::string Name, Type ValueType,
				FMaterialFunctionDefault Default, bool bAdvanced = false) -> Link
			{
				// Both PBR entry points retain the same identities for common inputs.
				const auto Port = Id(Family == Entry::StandardPBR_ORM ? Entry::StandardPBR : Family, Slot);
				Graph.Signature.Inputs.push_back({.Id = Port, .Type = ValueType,
					.Name = std::move(Name), .DisplayOrder = static_cast<int32>(Graph.Signature.Inputs.size()),
					.bAdvanced = bAdvanced, .Default = std::move(Default)});
				const auto Result = Node(Op::FunctionInput, ValueType);
				Graph.Nodes.back().FunctionPortId = Port;
				return Result;
			}
			auto Output(uint32 Slot, std::string Name, Type ValueType, Link Source) -> void
			{
				const auto Port = Id(Family, Slot);
				Graph.Signature.Outputs.push_back({.Id = Port, .Type = ValueType, .Name = std::move(Name),
					.DisplayOrder = static_cast<int32>(Graph.Signature.Outputs.size())});
				Node(Op::FunctionOutput, ValueType, {Source});
				Graph.Nodes.back().FunctionPortId = Port;
			}
			auto Constant(Type ValueType, float X, float Y = 0, float Z = 0) -> Link
			{
				const auto Result = Node(Op::Constant, ValueType);
				Graph.Nodes.back().Literal = {.X = X, .Y = Y, .Z = Z};
				return Result;
			}
			auto Swizzle(Link Source, Type ValueType, uint8 X, uint8 Y = 0, uint8 Z = 0) -> Link
			{
				const auto Result = Node(Op::Swizzle, ValueType, {Source});
				auto& N = Graph.Nodes.back();
				N.SwizzleLength = ValueType == Type::Float ? 1 : ValueType == Type::Float2 ? 2 : 3;
				N.SwizzleX = X; N.SwizzleY = Y; N.SwizzleZ = Z;
				return Result;
			}
			auto Call(DMaterialFunction* Function, std::vector<FMaterialFunctionInputBinding> Inputs) -> Link
			{
				const auto Result = Node(Op::FunctionCall, Function->GetFunctionSignature().Outputs.front().Type);
				FMaterialFunctionCall Call{.NodeId = Result.SourceNodeId, .Function = Function, .Inputs = std::move(Inputs)};
				for (const auto& Output : Function->GetFunctionSignature().Outputs)
					Call.Outputs.push_back({Output.Id, Output.Type});
				Graph.Calls.push_back(std::move(Call));
				return {.SourceNodeId = Result.SourceNodeId, .SourceOutputId = Graph.Calls.back().Outputs.front().OutputId};
			}
		};

		auto ComposeSurfaceValue(FBuilder& B, uint32 Role, Link Factor, Link Sample,
			bool bEvaluateAtRoot = false) -> Link
		{
			const auto ValueType = GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(Role));
			if (Role == 1) return B.Node(Op::BlendNormalsRNM, Type::Float3, {Factor, Sample});
			// Generated imports compose source values; the Engine root owns numerical
			// output policy. Reusable function implementations retain authored operations.
			if (bEvaluateAtRoot)
				return B.Node(Role == 5 ? Op::Add : Op::Multiply, ValueType, {Factor, Sample});
			if (Role == 5)
			{
				const auto Zero = B.Constant(Type::Float3, 0, 0, 0);
				Factor = B.Node(Op::Maximum, Type::Float3, {Factor, Zero});
				Sample = B.Node(Op::Maximum, Type::Float3, {Sample, Zero});
				return B.Node(Op::Add, Type::Float3, {Factor, Sample});
			}
			Factor = B.Node(Op::Saturate, ValueType, {Factor});
			if (Role != 0) Sample = B.Node(Op::Saturate, ValueType, {Sample});
			auto Value = B.Node(Op::Multiply, ValueType, {Factor, Sample});
			if (Role == 3)
			{
				const auto Min = B.Constant(Type::Float, .045f), Max = B.Constant(Type::Float, 1);
				Value = B.Node(Op::Clamp, Type::Float, {Value, Min, Max});
			}
			return Value;
		}
	}

	auto MakeStandardMaterialFunctionGraph(Entry Function, const FStandardMaterialFunctions& Dependencies)
		-> FMaterialFunctionGraph
	{
		FBuilder B{Function};
		if (Function == Entry::UVTransform)
		{
			const auto UV = B.Input(1, "UV", Type::Float2, {.Kind = Kind::UV0});
			const auto Scale = B.Input(2, "Scale", Type::Float2, Numeric(1, 1));
			const auto Offset = B.Input(3, "Offset", Type::Float2, Numeric(0, 0));
			const auto Angle = B.Input(4, "Rotation", Type::Float, Numeric(0));
			const auto Scaled = B.Node(Op::Multiply, Type::Float2, {UV, Scale});
			const auto Sine = B.Node(Op::Sine, Type::Float, {Angle});
			const auto Cosine = B.Node(Op::Cosine, Type::Float, {Angle});
			const auto X = B.Swizzle(Scaled, Type::Float, 0), Y = B.Swizzle(Scaled, Type::Float, 1);
			const auto CX = B.Node(Op::Multiply, Type::Float, {Cosine, X});
			const auto SY = B.Node(Op::Multiply, Type::Float, {Sine, Y});
			const auto SX = B.Node(Op::Multiply, Type::Float, {Sine, X});
			const auto CY = B.Node(Op::Multiply, Type::Float, {Cosine, Y});
			const auto RX = B.Node(Op::Subtract, Type::Float, {CX, SY});
			const auto RY = B.Node(Op::Add, Type::Float, {SX, CY});
			const auto Rotated = B.Node(Op::MakeFloat2, Type::Float2, {RX, RY});
			B.Output(100, "UV", Type::Float2, B.Node(Op::Add, Type::Float2, {Rotated, Offset}));
		}
		else if (Function == Entry::SampleNormal || Function == Entry::SampleORM)
		{
			const bool bNormal = Function == Entry::SampleNormal;
			const auto Tex = B.Input(1, "Texture", Type::Texture2D,
				Texture(bNormal ? EMaterialTextureFallback::FlatRGNormal : EMaterialTextureFallback::White));
			const auto UV = B.Input(2, "UV", Type::Float2, {.Kind = Kind::UV0});
			const auto Sample = B.Node(Op::TextureSample2D, Type::Float4, {Tex, UV});
			if (bNormal)
			{
				const auto Strength = B.Input(3, "Strength", Type::Float, Numeric(1));
				const auto Normal = B.Input(4, "Normal", Type::Float3, Numeric(0, 0, 1));
				const auto RG = B.Swizzle(Sample, Type::Float2, 0, 1);
				// Scale encoded RG about its neutral midpoint before the existing safe decoder.
				const auto Half = B.Constant(Type::Float2, .5f, .5f);
				const auto Centered = B.Node(Op::Subtract, Type::Float2, {RG, Half});
				const auto Strength2 = B.Node(Op::Splat2, Type::Float2, {Strength});
				const auto Scaled = B.Node(Op::Multiply, Type::Float2, {Centered, Strength2});
				const auto Encoded = B.Node(Op::Add, Type::Float2, {Scaled, Half});
				const auto Decoded = B.Node(Op::DecodeNormalRG, Type::Float3, {Encoded});
				B.Output(100, "Normal", Type::Float3, B.Node(Op::BlendNormalsRNM, Type::Float3, {Normal, Decoded}));
			}
			else
			{
				B.Output(100, "Occlusion", Type::Float, B.Swizzle(Sample, Type::Float, 0));
				B.Output(101, "Roughness", Type::Float, B.Swizzle(Sample, Type::Float, 1));
				B.Output(102, "Metallic", Type::Float, B.Swizzle(Sample, Type::Float, 2));
			}
		}
		else if (Function == Entry::DecodeImportedNormalRG)
		{
			const auto RG = B.Input(1, "RG", Type::Float2, Numeric(.5f, .5f));
			// Retain the shipped SampleNormal strength-one arithmetic exactly.
			const auto Half = B.Constant(Type::Float2, .5f, .5f);
			const auto Centered = B.Node(Op::Subtract, Type::Float2, {RG, Half});
			const auto Strength = B.Constant(Type::Float, 1);
			const auto Strength2 = B.Node(Op::Splat2, Type::Float2, {Strength});
			const auto Scaled = B.Node(Op::Multiply, Type::Float2, {Centered, Strength2});
			const auto Encoded = B.Node(Op::Add, Type::Float2, {Scaled, Half});
			B.Output(100, "Normal", Type::Float3, B.Node(Op::DecodeNormalRG, Type::Float3, {Encoded}));
		}
		else
		{
			const bool bPacked = Function == Entry::StandardPBR_ORM;
			const bool bValues = Function == Entry::ImportedSurfaceValues;
			if (!bValues) B.Input(1, "UV", Type::Float2, {.Kind = Kind::UV0});
			std::array<Link, 8> Factors, Textures, UVs, Values;
			for (uint32 I = 0; I < 8; ++I)
			{
				const auto ValueType = GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(I));
				const auto Default = I == 0 ? Numeric(.5f, .5f, .5f) : I == 1 ? Numeric(0, 0, 1)
					: I == 3 ? Numeric(.5f) : I == 4 || I >= 6 ? Numeric(1) : Numeric(0);
				Factors[I] = B.Input(10 + I, RoleNames[I], ValueType, Default, I == 1 || I >= 4);
				if (bValues)
				{
					Textures[I] = B.Input(20 + I, std::string(RoleNames[I]) + "Sample", ValueType,
						I == 1 ? Numeric(0, 0, 1) : I == 5 ? Numeric(0, 0, 0) : Numeric(1, 1, 1));
					continue;
				}
				if (bPacked && I >= 2 && I <= 4) continue;
				Textures[I] = B.Input(20 + I, std::string(RoleNames[I]) + "Texture", Type::Texture2D,
					Texture(I == 1 ? EMaterialTextureFallback::FlatRGNormal : I == 5
						? EMaterialTextureFallback::Black : EMaterialTextureFallback::White), I >= 2);
				UVs[I] = B.Input(30 + I, std::string(RoleNames[I]) + "UV", Type::Float2,
					{.Kind = Kind::Input, .InputId = Id(Entry::StandardPBR, 1)}, true);
			}
			Link ORM;
			if (bPacked)
			{
				const auto Tex = B.Input(40, "ORMTexture", Type::Texture2D, Texture());
				const auto UV = B.Input(41, "ORMUV", Type::Float2,
					{.Kind = Kind::Input, .InputId = Id(Entry::StandardPBR, 1)}, true);
				ORM = B.Call(Dependencies.SampleORM.Get(), {{Id(Entry::SampleORM, 1), Type::Texture2D, Tex},
					{Id(Entry::SampleORM, 2), Type::Float2, UV}});
			}
			constexpr std::array<uint8, 8> Channels{0, 0, 2, 1, 0, 0, 3, 0};
			for (uint32 I = 0; I < 8; ++I)
			{
				if (I == 1)
				{
					if (bValues)
					{
						Values[I] = ComposeSurfaceValue(B, I, Factors[I], Textures[I]);
						continue;
					}
					Values[I] = B.Call(Dependencies.SampleNormal.Get(), {
						{Id(Entry::SampleNormal, 1), Type::Texture2D, Textures[I]},
						{Id(Entry::SampleNormal, 2), Type::Float2, UVs[I]},
						{Id(Entry::SampleNormal, 4), Type::Float3, Factors[I]}});
					continue;
				}
				const auto ValueType = GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(I));
				Link Channel;
				if (bValues) Channel = Textures[I];
				else if (bPacked && I >= 2 && I <= 4)
					Channel = {.SourceNodeId = ORM.SourceNodeId, .SourceOutputId = Id(Entry::SampleORM, 100 + Channels[I])};
				else
				{
					const auto Sample = B.Node(Op::TextureSample2D, Type::Float4, {Textures[I], UVs[I]});
					Channel = B.Swizzle(Sample, ValueType, Channels[I], 1, 2);
				}
				Values[I] = ComposeSurfaceValue(B, I, Factors[I], Channel);
			}
			B.Output(100, "Surface", Type::Surface, B.Node(Op::MakeSurface, Type::Surface, {Values.begin(), Values.end()}));
		}
		return std::move(B.Graph);
	}

	auto EnsureStandardMaterialFunctions(FStandardMaterialFunctions& OutFunctions, std::string& OutError) -> bool
	{
		FStandardMaterialFunctions Result;
		const std::array Slots{&Result.UVTransform, &Result.SampleNormal, &Result.SampleORM,
			&Result.StandardPBR, &Result.StandardPBR_ORM, &Result.ImportedSurfaceValues, &Result.DecodeImportedNormalRG};
		for (uint32 I = 0; I < Slots.size(); ++I)
		{
			const auto EntryKind = static_cast<Entry>(I + 1);
			const std::string Source = std::format("Durin.MaterialFunctions.{}", EntryNames[I]);
			FPackagePath Path;
			if (!FPackagePath::TryCreate(std::format("/Engine/Materials/Functions/{}", EntryNames[I]), Path, &OutError)) return false;
			DMaterialFunction* Function = nullptr;
			DPackage* Package = FindResidentPackage(Path);
			if (Package) Function = Cast<DMaterialFunction>(Package->FindTopLevelAsset(FName(Path.GetPackageName())));
			else if (FindAssetExact(Path))
			{
				FObjectPath ObjectPath;
				if (!FObjectPath::TryCreate(std::format("{}.{}", Path.ToString(), Path.GetPackageName()), ObjectPath, &OutError)) return false;
				const auto Loaded = LoadObject(ObjectPath, Function);
				if (!Loaded) { OutError = Loaded.Message; return false; }
				Package = Function->GetPackage();
			}
			const auto Expected = MakeStandardMaterialFunctionGraph(EntryKind, Result);
			if (Package)
			{
				if (!Function || Function->GetAuthoringSource() != Source
					|| Function->GetAuthoringSourceVersion() != StandardMaterialFunctionVersion
					|| Function->GetFunctionSignature() != Expected.Signature)
				{
					OutError = std::format("Standard function {} has incompatible provenance or an edited interface; preserve it and resolve the conflict before importing.", Path.ToString());
					return false;
				}
			}
			else
			{
				FTopLevelAssetPath AssetPath;
				if (!FTopLevelAssetPath::TryCreate(Path, Path.GetPackageName(), AssetPath)) return false;
				const auto Created = IAssetTools::Get().CreateAsset(AssetPath, DMaterialFunction::StaticClass());
				Function = Cast<DMaterialFunction>(Created.Asset);
				if (!Created || !Function) { OutError = Created.Message; return false; }
				const auto Applied = Function->SetFunctionGraph(Expected);
				if (!Applied)
				{
					OutError = Applied.Diagnostics.empty() ? "Standard function graph is invalid." : Applied.Diagnostics.front().Message;
					UnloadPackage(Function->GetPackage(), EAssetPackageUnloadPolicy::DiscardUnsaved);
					return false;
				}
				Function->SetAuthoringSource(Source, StandardMaterialFunctionVersion);
				FMaterialFunctionPresentation Presentation;
				for (uint32 N = 0; N < Expected.Nodes.size(); ++N)
					Presentation.Nodes.push_back({Expected.Nodes[N].Id, static_cast<int32>(N % 6) * 320, static_cast<int32>(N / 6) * 240});
				Function->SetFunctionPresentation(std::move(Presentation));
				const auto Saved = SavePackage(Function->GetPackage());
				if (!Saved)
				{
					OutError = Saved.Message;
					UnloadPackage(Function->GetPackage(), EAssetPackageUnloadPolicy::DiscardUnsaved);
					return false;
				}
			}
			*Slots[I] = Function;
		}
		OutFunctions = Result;
		OutError.clear();
		return true;
	}

	auto MakeImportedSurfaceFunctionProgram(const FStandardMaterialFunctions& Functions,
		std::vector<FMaterialFunctionCall>& OutCalls, FMaterialGraphPresentation& OutPresentation) -> FMaterialProgram
	{
		FBuilder B{Entry::ImportedSurfaceValues};
		const auto Recipe = MakePBRMaterialParameterDefinitions();
		FMaterialGraphPresentation Presentation{.bHasMaterialOutputPosition = true, .MaterialOutputX = 2000, .MaterialOutputY = 400};
		FMaterialProgram Result;
		using ParameterKind = MaterialParameters::EMaterialBuiltinParameterKind;
		constexpr std::array<uint8, 8> Channels{1, 6, 4, 3, 2, 1, 5, 2};
		for (uint32 I = 0; I < 8; ++I)
		{
			const auto Role = static_cast<EMaterialSurfaceOutput>(I);
			const auto Parameter = [&](ParameterKind Kind, Type ValueType, int32 Column, int32 Row) {
				const auto Id = GetMaterialSurfaceParameterId(Role, Kind);
				const auto Definition = std::ranges::find(Recipe, Id, &FMaterialParameterDefinition::Id);
				require(Definition != Recipe.end());
				const auto Link = B.Node(ValueType == Type::Texture2D ? Op::TextureParameter : Op::Parameter, ValueType);
				B.Graph.Nodes.back().Parameter = *Definition;
				Presentation.Nodes.push_back({Link.SourceNodeId, Column * 320, static_cast<int32>(I) * 600 + Row * 130});
				return Link;
			};
			const auto Factor = Parameter(ParameterKind::Value, GetMaterialSurfaceOutputType(Role), 2, 0);
			const auto Channel = Parameter(ParameterKind::UVChannel, Type::Float, -2, 0);
			const auto Scale = Parameter(ParameterKind::UVScale, Type::Float2, -2, 1);
			const auto Offset = Parameter(ParameterKind::UVOffset, Type::Float2, -2, 2);
			const auto Rotation = Parameter(ParameterKind::UVRotation, Type::Float, -2, 3);
			const auto UV = B.Node(Op::TextureCoordinates, Type::Float2, {Channel, Scale, Offset, Rotation});
			Presentation.Nodes.push_back({UV.SourceNodeId, -320, static_cast<int32>(I) * 600});
			auto Sample = B.Node(Op::TextureSampleParameter2D, Type::Float4, {UV});
			const auto TextureId = GetMaterialSurfaceParameterId(Role, ParameterKind::Texture);
			B.Graph.Nodes.back().Parameter = *std::ranges::find(Recipe, TextureId, &FMaterialParameterDefinition::Id);
			Presentation.Nodes.push_back({Sample.SourceNodeId, 0, static_cast<int32>(I) * 600});
			Sample.SourceOutputIndex = Channels[I];
			if (I == 1)
			{
				Sample = B.Call(Functions.DecodeImportedNormalRG.Get(), {{Id(Entry::DecodeImportedNormalRG, 1), Type::Float2, Sample}});
				Presentation.Nodes.push_back({Sample.SourceNodeId, 320, static_cast<int32>(I) * 600 + 150});
			}
			const auto FirstCompositionNode = B.Graph.Nodes.size();
			GetMaterialSurfaceOutputLink(Result.Outputs, Role) = ComposeSurfaceValue(B, I, Factor, Sample, true);
			for (size_t N = FirstCompositionNode; N < B.Graph.Nodes.size(); ++N)
				Presentation.Nodes.push_back({B.Graph.Nodes[N].Id,
					960 + static_cast<int32>((N - FirstCompositionNode) % 3) * 320,
					static_cast<int32>(I) * 600 + static_cast<int32>((N - FirstCompositionNode) / 3) * 170});
		}
		Result.Nodes = std::move(B.Graph.Nodes);
		OutCalls = std::move(B.Graph.Calls);
		OutPresentation = std::move(Presentation);
		return Result;
	}
}
