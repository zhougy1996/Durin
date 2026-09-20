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
		constexpr std::array EntryNames{"UVTransform", "SampleNormal", "SampleORM"};
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
				if constexpr (std::is_same_v<T, DMaterialExpressionMakeVector2>)
					{ N->X = Inputs[0]; N->Y = Inputs[1]; }
				if constexpr (std::is_same_v<T, DMaterialExpressionTextureSample2D>)
					{ N->Texture = Inputs[0]; N->UV = Inputs[1]; }
				return {N->Id};
			}
			auto Input(uint32 Slot) -> Link
			{
				auto* N = Add<DMaterialExpressionFunctionInput>();
				const auto Port = Id(Family, Slot);
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
		};
	}

	auto MakeStandardMaterialFunctionExpressions(Entry Function)
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
				Detail->A.SetConstant({0, 0, 1});
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
		const std::array Slots{&Result.UVTransform, &Result.SampleNormal, &Result.SampleORM};
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
