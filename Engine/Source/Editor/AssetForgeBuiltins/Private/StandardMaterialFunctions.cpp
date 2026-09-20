#include "AssetForge/Builtins/StandardMaterialFunctions.h"

#include "Asset/Asset.h"
#include "Asset/PackageSerialization.h"
#include "DObject/Package.h"
#include "DObject/Class.h"
#include "DObject/Property.h"

#include <algorithm>

namespace Durin::AssetForge::Builtins
{
	namespace
	{
		using Type = EMaterialProgramValueType;
		using Entry = EStandardMaterialFunction;
		using Link = FMaterialExpressionInput;
		constexpr std::array EntryNames{"UVTransform", "SampleNormal", "SampleORM", "StandardPBR", "StandardPBR_ORM"};
		constexpr auto Id(Entry Function, uint32 Slot) -> FGuid { return StandardMaterialPortId(Function, Slot); }

		struct FBuilder
		{
			Entry Family;
			FStandardMaterialFunctionExpressions Recipe;
			FMaterialFunctionSignature Interface = GetStandardMaterialFunctionInterface(Family);
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
			auto Input(uint32 Slot) -> Link
			{
				auto* N = Add<DMaterialExpressionFunctionInput>();
				const auto Port = Id(Family == Entry::StandardPBR_ORM ? Entry::StandardPBR : Family, Slot);
				const auto It = std::ranges::find(Interface.Inputs, Port, &FMaterialFunctionPort::Id);
				check(It != Interface.Inputs.end());
				N->Port = *It;
				return {N->Id};
			}
			auto Output(uint32 Slot, Link Source) -> void
			{
				auto* N = Add<DMaterialExpressionFunctionOutput>();
				const auto It = std::ranges::find(Interface.Outputs, Id(Family, Slot), &FMaterialFunctionPort::Id);
				check(It != Interface.Outputs.end());
				N->Port = *It;
				N->Source = Source;
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
				// Share the zero between both bounds instead of emitting duplicate inline constants.
				auto* Zero = B.Add<DMaterialExpressionVector3Constant>();
				Zero->Value = {0, 0, 0};
				Factor = B.Node<DMaterialExpressionMaximum>(Type::Float3, {Factor, {Zero->Id}});
				Sample = B.Node<DMaterialExpressionMaximum>(Type::Float3, {Sample, {Zero->Id}});
				return B.Node<DMaterialExpressionAdd>(Type::Float3, {Factor, Sample});
			}
			Factor = B.Node<DMaterialExpressionSaturate>(ValueType, {Factor});
			if (Role != 0) Sample = B.Node<DMaterialExpressionSaturate>(ValueType, {Sample});
			auto Value = B.Node<DMaterialExpressionMultiply>(ValueType, {Factor, Sample});
			if (Role == 3)
			{
				auto* N = B.Add<DMaterialExpressionClamp>();
				N->Input = Value;
				N->MinimumDefault = {.045f};
				N->MaximumDefault = {1};
				Value = {N->Id};
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
			const auto UV = B.Input(1);
			const auto Scale = B.Input(2);
			const auto Offset = B.Input(3);
			const auto Angle = B.Input(4);
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
			B.Output(100, B.Node<DMaterialExpressionAdd>(Type::Float2, {Rotated, Offset}));
		}
		else if (Function == Entry::SampleNormal || Function == Entry::SampleORM)
		{
			const bool bNormal = Function == Entry::SampleNormal;
			const auto Tex = B.Input(1);
			const auto UV = B.Input(2);
			const auto Sample = B.Node<DMaterialExpressionTextureSample2D>(Type::Float4, {Tex, UV});
			if (bNormal)
			{
				const auto Strength = B.Input(3);
				const auto Normal = B.Input(4);
				const Link Decoded{Sample.ExpressionId, 1};
				// Strength acts on decoded normals; RNM safely normalizes the result.
				auto* Detail = B.Add<DMaterialExpressionLerp>();
				Detail->ResultType = Type::Float3;
				Detail->ADefault = {0, 0, 1};
				Detail->B = Decoded;
				Detail->Alpha = Strength;
				B.Output(100, B.Node<DMaterialExpressionBlendNormalsRNM>(Type::Float3, {Normal, {Detail->Id}}));
			}
			else
			{
				B.Output(100, {Sample.ExpressionId, 2});
				B.Output(101, {Sample.ExpressionId, 3});
				B.Output(102, {Sample.ExpressionId, 4});
			}
		}
		else
		{
			const bool bPacked = Function == Entry::StandardPBR_ORM;
			B.Input(1);
			std::array<Link, 8> Factors, Textures, UVs, Values;
			for (uint32 I = 0; I < 8; ++I)
			{
				Factors[I] = B.Input(10 + I);
				if (bPacked && I >= 2 && I <= 4) continue;
				Textures[I] = B.Input(20 + I);
				UVs[I] = B.Input(30 + I);
			}
			Link ORM;
			if (bPacked)
			{
				const auto Tex = B.Input(40);
				const auto UV = B.Input(41);
				ORM = B.Call(Dependencies.SampleORM.Get(), {{Id(Entry::SampleORM, 1), Type::Texture2D, Tex},
					{Id(Entry::SampleORM, 2), Type::Float2, UV}});
			}
			constexpr std::array<uint8, 8> Channels{0, 0, 2, 1, 0, 0, 3, 0};
			for (uint32 I = 0; I < 8; ++I)
			{
				if (I == 1)
				{
					Values[I] = B.Call(Dependencies.SampleNormal.Get(), {
						{Id(Entry::SampleNormal, 1), Type::Texture2D, Textures[I]},
						{Id(Entry::SampleNormal, 2), Type::Float2, UVs[I]},
						{Id(Entry::SampleNormal, 4), Type::Float3, Factors[I]}});
					continue;
				}
				const auto ValueType = GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(I));
				Link Channel;
				if (bPacked && I >= 2 && I <= 4)
					Channel = {.ExpressionId = ORM.ExpressionId, .OutputId = Id(Entry::SampleORM, 100 + Channels[I])};
				else
				{
					const auto Sample = B.Node<DMaterialExpressionTextureSample2D>(Type::Float4, {Textures[I], UVs[I]});
					// Color roles read raw RGB even if a caller supplies a Normal-usage texture.
					// The sampler's RGB output decodes normals, so only scalar masks are redundant here.
					Channel = ValueType == Type::Float3 ? B.Swizzle(Sample, ValueType, Channels[I], 1, 2)
						: Link{Sample.ExpressionId, static_cast<uint8>(Channels[I] + 2)};
				}
				Values[I] = ComposeSurfaceValue(B, I, Factors[I], Channel);
			}
			B.Output(100, B.Node<DMaterialExpressionMakeSurface>(Type::Surface, {Values.begin(), Values.end()}));
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
					bEqual = ArePropertyValuesIdentical(Property, A, I, B, I);
			});
			if (!bEqual) return false;
		}
		return true;
	}

	auto LoadStandardMaterialFunctions(FStandardMaterialFunctions& OutFunctions, std::string& OutError) -> bool
	{
		FStandardMaterialFunctions Result;
		const std::array Slots{&Result.UVTransform, &Result.SampleNormal, &Result.SampleORM,
			&Result.StandardPBR, &Result.StandardPBR_ORM};
		for (uint32 I = 0; I < Slots.size(); ++I)
		{
			const auto EntryKind = static_cast<Entry>(I + 1);
			FPackagePath Path;
			if (const auto PathValidation = FPackagePath::TryCreateWithDiagnostic(std::format("/Engine/Materials/Functions/{}", EntryNames[I]), Path); !PathValidation) { OutError = Durin::FormatObjectError(PathValidation.Error); return false; }
			DMaterialFunction* Function = nullptr;
			DPackage* Package = FindResidentPackage(Path);
			if (Package) Function = Cast<DMaterialFunction>(Package->FindTopLevelAsset(FName(Path.GetPackageName())));
			else if (FindAssetExact(Path))
			{
				FObjectPath ObjectPath;
				if (const auto PathValidation = FObjectPath::TryCreateWithDiagnostic(std::format("{}.{}", Path.ToString(), Path.GetPackageName()), ObjectPath); !PathValidation) { OutError = Durin::FormatObjectError(PathValidation.Error); return false; }
				const auto Loaded = LoadObject(ObjectPath, Function);
				if (!Loaded) { OutError = Loaded.Message; return false; }
				Package = Function->GetPackage();
			}
			if (!Package)
			{
				OutError = std::format("Missing standard function {}; restore the shipped Engine content.", Path.ToString());
				return false;
			}
			const auto Expected = GetStandardMaterialFunctionInterface(EntryKind);
			if (!Function || Function->GetFunctionSignature() != Expected)
			{
				OutError = std::format("Standard function {} has an incompatible interface; preserve it and resolve the conflict before use.", Path.ToString());
				return false;
			}
			*Slots[I] = Function;
		}
		OutFunctions = Result;
		OutError.clear();
		return true;
	}

}
