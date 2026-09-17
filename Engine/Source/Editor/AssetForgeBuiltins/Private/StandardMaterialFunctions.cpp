#include "AssetForge/Builtins/StandardMaterialFunctions.h"

#include "Asset/Asset.h"
#include "Asset/PackageSerialization.h"
#include "AssetTools/IAssetTools.h"
#include "DObject/Package.h"
#include "DObject/Class.h"
#include "DObject/Property.h"

#include <algorithm>

namespace Durin::AssetForge::Builtins
{
	namespace
	{
		using Type = EMaterialProgramValueType;
		using Kind = EMaterialFunctionDefaultKind;
		using Entry = EStandardMaterialFunction;
		using Link = FMaterialExpressionInput;
		constexpr std::array RoleNames{"BaseColor", "Normal", "Metallic", "Roughness",
			"AmbientOcclusion", "Emissive", "Opacity", "OpacityMask"};
		constexpr std::array EntryNames{"UVTransform", "SampleNormal", "SampleORM", "StandardPBR", "StandardPBR_ORM", "ImportedSurfaceValues"};
		constexpr auto Id(Entry Function, uint32 Slot) -> FGuid { return StandardMaterialPortId(Function, Slot); }
		auto Numeric(float X, float Y = 0, float Z = 0) -> FMaterialFunctionDefault
			{ return {.Kind = Kind::Numeric, .Numeric = {.X = X, .Y = Y, .Z = Z}}; }
		auto Texture(EMaterialTextureFallback Fallback = EMaterialTextureFallback::White)
			-> FMaterialFunctionDefault { return {.Kind = Kind::Texture, .TextureFallback = Fallback}; }

		struct FBuilder
		{
			Entry Family;
			FStandardMaterialFunctionExpressions Recipe;
			int32 InputCount = 0, OutputCount = 0;
			template<typename T>
			auto Add() -> T*
			{
				auto* Expression = NewObject<T>(nullptr, NAME_None);
				Expression->Id = {0xf67a24b1, 0x4378491a, static_cast<uint32>(Family),
					static_cast<uint32>(Recipe.Expressions.size() + 1)};
				Recipe.Expressions.emplace_back(Expression);
				return Expression;
			}
			template<typename T>
			auto Node(Type ResultType, std::vector<Link> Inputs) -> Link
			{
				auto* N = Add<T>();
				check(Inputs.size() == N->GetAuthoredInputCount());
				if constexpr (requires { N->ResultType; }) N->ResultType = ResultType;
				if constexpr (requires { N->A; N->B; }) { N->A = Inputs[0]; N->B = Inputs[1]; }
				if constexpr (std::is_same_v<T, DMaterialExpressionBlendNormalsRNM>)
					{ N->Base = Inputs[0]; N->Detail = Inputs[1]; }
				if constexpr (std::is_same_v<T, DMaterialExpressionLerp>) N->Alpha = Inputs[2];
				if constexpr (requires { N->Input; }) N->Input = Inputs[0];
				if constexpr (std::is_same_v<T, DMaterialExpressionClamp>)
					{ N->Minimum = Inputs[1]; N->Maximum = Inputs[2]; }
				if constexpr (std::is_same_v<T, DMaterialExpressionMakeVector2>)
					{ N->X = Inputs[0]; N->Y = Inputs[1]; }
				if constexpr (std::is_same_v<T, DMaterialExpressionTextureSample2D>)
					{ N->Texture = Inputs[0]; N->UV = Inputs[1]; }
				if constexpr (std::is_same_v<T, DMaterialExpressionMakeSurface>)
				{
					N->BaseColor = Inputs[0]; N->Normal = Inputs[1]; N->Metallic = Inputs[2];
					N->Roughness = Inputs[3]; N->AmbientOcclusion = Inputs[4]; N->Emissive = Inputs[5];
					N->Opacity = Inputs[6]; N->OpacityMask = Inputs[7];
				}
				return {N->Id};
			}
			auto Input(uint32 Slot, std::string Name, Type ValueType,
				FMaterialFunctionDefault Default, bool bAdvanced = false) -> Link
			{
				auto* N = Add<DMaterialExpressionFunctionInput>();
				const auto Port = Id(Family == Entry::StandardPBR_ORM ? Entry::StandardPBR : Family, Slot);
				N->Port = {.Id = Port, .Type = ValueType,
					.Name = std::move(Name), .DisplayOrder = InputCount++,
					.bAdvanced = bAdvanced, .Default = std::move(Default)};
				return {N->Id};
			}
			auto Output(uint32 Slot, std::string Name, Type ValueType, Link Source) -> void
			{
				auto* N = Add<DMaterialExpressionFunctionOutput>();
				const auto Port = Id(Family, Slot);
				N->Port = {.Id = Port, .Type = ValueType, .Name = std::move(Name),
					.DisplayOrder = OutputCount++};
				N->Source = Source;
			}
			auto Constant(Type ValueType, float X, float Y = 0, float Z = 0) -> Link
			{
				if (ValueType == Type::Float)
				{
					auto* N = Add<DMaterialExpressionScalarConstant>(); N->Value = X; return {N->Id};
				}
				if (ValueType == Type::Float2)
				{
					auto* N = Add<DMaterialExpressionVector2Constant>(); N->Value = {X, Y}; return {N->Id};
				}
				check(ValueType == Type::Float3);
				auto* N = Add<DMaterialExpressionVector3Constant>(); N->Value = {X, Y, Z}; return {N->Id};
			}
			auto Swizzle(Link Source, Type ValueType, uint8 X, uint8 Y = 0, uint8 Z = 0) -> Link
			{
				auto* N = Add<DMaterialExpressionSwizzle>();
				N->Input = Source;
				N->Components = {X};
				if (ValueType != Type::Float) N->Components.push_back(Y);
				if (ValueType == Type::Float3) N->Components.push_back(Z);
				return {N->Id};
			}
			auto Call(DMaterialFunction* Function, std::vector<FMaterialExpressionFunctionInputBinding> Inputs) -> Link
			{
				auto* N = Add<DMaterialExpressionFunctionCall>();
				N->Function = Function; N->Inputs = std::move(Inputs);
				for (const auto& Output : Function->GetFunctionSignature().Outputs)
					N->Outputs.push_back({Output.Id, Output.Type});
				return {N->Id, 0, N->Outputs.front().OutputId};
			}
		};

		auto ComposeSurfaceValue(FBuilder& B, uint32 Role, Link Factor, Link Sample) -> Link
		{
			const auto ValueType = GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(Role));
			if (Role == 1) return B.Node<DMaterialExpressionBlendNormalsRNM>(Type::Float3, {Factor, Sample});
			if (Role == 5)
			{
				const auto Zero = B.Constant(Type::Float3, 0, 0, 0);
				Factor = B.Node<DMaterialExpressionMaximum>(Type::Float3, {Factor, Zero});
				Sample = B.Node<DMaterialExpressionMaximum>(Type::Float3, {Sample, Zero});
				return B.Node<DMaterialExpressionAdd>(Type::Float3, {Factor, Sample});
			}
			Factor = B.Node<DMaterialExpressionSaturate>(ValueType, {Factor});
			if (Role != 0) Sample = B.Node<DMaterialExpressionSaturate>(ValueType, {Sample});
			auto Value = B.Node<DMaterialExpressionMultiply>(ValueType, {Factor, Sample});
			if (Role == 3)
			{
				const auto Min = B.Constant(Type::Float, .045f), Max = B.Constant(Type::Float, 1);
				Value = B.Node<DMaterialExpressionClamp>(Type::Float, {Value, Min, Max});
			}
			return Value;
		}
	}

	auto MakeStandardMaterialFunctionExpressions(Entry Function, const FStandardMaterialFunctions& Dependencies)
		-> FStandardMaterialFunctionExpressions
	{
		FBuilder B{Function};
		if (Function == Entry::UVTransform)
		{
			const auto UV = B.Input(1, "UV", Type::Float2, {.Kind = Kind::UV0});
			const auto Scale = B.Input(2, "Scale", Type::Float2, Numeric(1, 1));
			const auto Offset = B.Input(3, "Offset", Type::Float2, Numeric(0, 0));
			const auto Angle = B.Input(4, "Rotation", Type::Float, Numeric(0));
			const auto Scaled = B.Node<DMaterialExpressionMultiply>(Type::Float2, {UV, Scale});
			const auto Sine = B.Node<DMaterialExpressionSine>(Type::Float, {Angle});
			const auto Cosine = B.Node<DMaterialExpressionCosine>(Type::Float, {Angle});
			const auto X = B.Swizzle(Scaled, Type::Float, 0), Y = B.Swizzle(Scaled, Type::Float, 1);
			const auto CX = B.Node<DMaterialExpressionMultiply>(Type::Float, {Cosine, X});
			const auto SY = B.Node<DMaterialExpressionMultiply>(Type::Float, {Sine, Y});
			const auto SX = B.Node<DMaterialExpressionMultiply>(Type::Float, {Sine, X});
			const auto CY = B.Node<DMaterialExpressionMultiply>(Type::Float, {Cosine, Y});
			const auto RX = B.Node<DMaterialExpressionSubtract>(Type::Float, {CX, SY});
			const auto RY = B.Node<DMaterialExpressionAdd>(Type::Float, {SX, CY});
			const auto Rotated = B.Node<DMaterialExpressionMakeVector2>(Type::Float2, {RX, RY});
			B.Output(100, "UV", Type::Float2, B.Node<DMaterialExpressionAdd>(Type::Float2, {Rotated, Offset}));
		}
		else if (Function == Entry::SampleNormal || Function == Entry::SampleORM)
		{
			const bool bNormal = Function == Entry::SampleNormal;
			const auto Tex = B.Input(1, "Texture", Type::Texture2D,
				Texture(bNormal ? EMaterialTextureFallback::FlatRGNormal : EMaterialTextureFallback::White));
			const auto UV = B.Input(2, "UV", Type::Float2, {.Kind = Kind::UV0});
			const auto Sample = B.Node<DMaterialExpressionTextureSample2D>(Type::Float4, {Tex, UV});
			if (bNormal)
			{
				const auto Strength = B.Input(3, "Strength", Type::Float, Numeric(1));
				const auto Normal = B.Input(4, "Normal", Type::Float3, Numeric(0, 0, 1));
				const Link Decoded{Sample.ExpressionId, 1};
				const auto Flat = B.Constant(Type::Float3, 0, 0, 1);
				// Strength acts on decoded normals; RNM safely normalizes the result.
				const auto Detail = B.Node<DMaterialExpressionLerp>(Type::Float3, {Flat, Decoded, Strength});
				B.Output(100, "Normal", Type::Float3, B.Node<DMaterialExpressionBlendNormalsRNM>(Type::Float3, {Normal, Detail}));
			}
			else
			{
				B.Output(100, "Occlusion", Type::Float, B.Swizzle(Sample, Type::Float, 0));
				B.Output(101, "Roughness", Type::Float, B.Swizzle(Sample, Type::Float, 1));
				B.Output(102, "Metallic", Type::Float, B.Swizzle(Sample, Type::Float, 2));
			}
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
					Channel = {.ExpressionId = ORM.ExpressionId, .OutputId = Id(Entry::SampleORM, 100 + Channels[I])};
				else
				{
					const auto Sample = B.Node<DMaterialExpressionTextureSample2D>(Type::Float4, {Textures[I], UVs[I]});
					Channel = B.Swizzle(Sample, ValueType, Channels[I], 1, 2);
				}
				Values[I] = ComposeSurfaceValue(B, I, Factors[I], Channel);
			}
			B.Output(100, "Surface", Type::Surface, B.Node<DMaterialExpressionMakeSurface>(Type::Surface, {Values.begin(), Values.end()}));
		}
		return std::move(B.Recipe);
	}

	auto FStandardMaterialFunctionExpressions::GetSignature() const -> FMaterialFunctionSignature
	{
		std::vector<DMaterialExpression*> Nodes;
		for (const auto& Expression : Expressions) Nodes.push_back(Expression.Get());
		return DeriveMaterialFunctionSignature(Nodes);
	}

	auto FStandardMaterialFunctionExpressions::Apply(DMaterialFunction& Function) const -> FMaterialProgramValidationResult
	{
		std::vector<DMaterialExpression*> Nodes;
		for (const auto& Expression : Expressions) Nodes.push_back(Expression.Get());
		return Function.SetFunctionExpressions(Nodes);
	}

	auto FStandardMaterialFunctionExpressions::Matches(const DMaterialFunction& Function) const -> bool
	{
		const auto& Current = Function.GetExpressionCollection().Expressions;
		if (Expressions.size() != Current.size()) return false;
		for (size_t Index = 0; Index < Expressions.size(); ++Index)
		{
			const auto* A = Expressions[Index].Get(); const auto* B = Current[Index].Get();
			if (!A || !B || A->GetClass() != B->GetClass()) return false;
			bool bEqual = true;
			A->GetClass()->ForEachProperty([&](FProperty* Property) {
				for (uint32 I = 0; bEqual && I < Property->GetArrayDim(); ++I)
					bEqual = ComparePropertyValues(Property, A, I, B, I) == EPropertyIdentityResult::Identical;
			});
			if (!bEqual) return false;
		}
		return true;
	}

	auto EnsureStandardMaterialFunctions(FStandardMaterialFunctions& OutFunctions, std::string& OutError) -> bool
	{
		FStandardMaterialFunctions Result;
		const std::array Slots{&Result.UVTransform, &Result.SampleNormal, &Result.SampleORM,
			&Result.StandardPBR, &Result.StandardPBR_ORM, &Result.ImportedSurfaceValues};
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
			const auto Expected = MakeStandardMaterialFunctionExpressions(EntryKind, Result);
			if (Package)
			{
				if (!Function || Function->GetAuthoringSource() != Source
					|| Function->GetAuthoringSourceVersion() != StandardMaterialFunctionVersion
					|| Function->GetFunctionSignature() != Expected.GetSignature())
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
				const auto Applied = Expected.Apply(*Function);
				if (!Applied)
				{
					OutError = Applied.Diagnostics.empty() ? "Standard function graph is invalid." : Durin::FormatMaterialError(Applied.Diagnostics.front().Error);
					UnloadPackage(Function->GetPackage(), EAssetPackageUnloadPolicy::DiscardUnsaved);
					return false;
				}
				Function->SetAuthoringSource(Source, StandardMaterialFunctionVersion);
				FMaterialFunctionPresentation Presentation;
				for (uint32 N = 0; N < Expected.Expressions.size(); ++N)
					Presentation.Nodes.push_back({Expected.Expressions[N]->Id, static_cast<int32>(N % 6) * 320, static_cast<int32>(N / 6) * 240});
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

}
