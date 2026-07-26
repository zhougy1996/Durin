#pragma once

#include "AssetSystem.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/DObjectArray.h"
#include "DObject/Archive.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "EngineTestSupport.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "Engine/Engine.h"
#include "Editor/ReflectedPropertyEditing.h"
#include "Editor/ReflectedPropertyView.h"
#include "DObject/Class.h"
#include "StaticMeshMaterialSlotDetails.h"
#include "Workspace/LevelEditorContext.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "RendererModule.h"
#include "Scene.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Texture/Texture2D.h"
#include "Widgets/MaterialPreview.h"

#include <gtest/gtest.h>

namespace
{
	constexpr Durin::uint8 MaterialTexturePngBytes[] = {
		137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 2, 0, 0, 0, 1, 8, 6, 0, 0, 0, 244, 34, 127, 138,
		0, 0, 0, 17, 73, 68, 65, 84, 120, 156, 99, 248, 207, 192, 240, 159, 129, 129, 129, 1, 0, 12, 252, 1, 255, 253, 45, 119, 109,
		0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};

	auto WriteMaterialTextureFixture(const std::filesystem::path& Path) -> void
	{
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(MaterialTexturePngBytes), sizeof(MaterialTexturePngBytes));
	}

	auto RewriteSerializedFieldAsLegacyMap(
		std::vector<Durin::uint8>& Bytes,
		std::string_view CurrentName,
		std::string_view LegacyName
	) -> bool
	{
		const Durin::uint64 CurrentNameSize = CurrentName.size();
		size_t NameLengthOffset = std::string::npos;
		for (size_t Offset = 0; Offset + sizeof(CurrentNameSize) + CurrentName.size() <= Bytes.size(); ++Offset)
		{
			Durin::uint64 CandidateSize = 0;
			std::memcpy(&CandidateSize, Bytes.data() + Offset, sizeof(CandidateSize));
			if (CandidateSize == CurrentNameSize
				&& std::memcmp(Bytes.data() + Offset + sizeof(CandidateSize), CurrentName.data(), CurrentName.size()) == 0)
			{
				if (NameLengthOffset != std::string::npos) return false;
				NameLengthOffset = Offset;
			}
		}
		if (NameLengthOffset == std::string::npos) return false;

		const size_t NameOffset = NameLengthOffset + sizeof(Durin::uint64);
		Bytes.erase(Bytes.begin() + NameOffset, Bytes.begin() + NameOffset + CurrentName.size());
		Bytes.insert(Bytes.begin() + NameOffset, LegacyName.begin(), LegacyName.end());
		const Durin::uint64 LegacyNameSize = LegacyName.size();
		std::memcpy(Bytes.data() + NameLengthOffset, &LegacyNameSize, sizeof(LegacyNameSize));

		const size_t KindOffset = NameOffset + LegacyName.size();
		if (KindOffset + 1 + sizeof(Durin::uint64) > Bytes.size()) return false;
		Bytes[KindOffset] = static_cast<Durin::uint8>(Durin::DurinCodeGen::EPropertyGenFlags::Map);

		const size_t SignatureLengthOffset = KindOffset + 1;
		Durin::uint64 CurrentSignatureSize = 0;
		std::memcpy(&CurrentSignatureSize, Bytes.data() + SignatureLengthOffset, sizeof(CurrentSignatureSize));
		const size_t SignatureOffset = SignatureLengthOffset + sizeof(CurrentSignatureSize);
		if (CurrentSignatureSize > Bytes.size() - SignatureOffset) return false;

		constexpr std::string_view LegacySignature = "Map<legacy-string,legacy-value>";
		Bytes.erase(Bytes.begin() + SignatureOffset, Bytes.begin() + SignatureOffset + CurrentSignatureSize);
		Bytes.insert(Bytes.begin() + SignatureOffset, LegacySignature.begin(), LegacySignature.end());
		const Durin::uint64 LegacySignatureSize = LegacySignature.size();
		std::memcpy(Bytes.data() + SignatureLengthOffset, &LegacySignatureSize, sizeof(LegacySignatureSize));
		return true;
	}

	auto ContainsSerializedField(std::span<const Durin::uint8> Bytes, std::string_view Name) -> bool
	{
		const Durin::uint64 NameSize = Name.size();
		for (size_t Offset = 0; Offset + sizeof(NameSize) + Name.size() <= Bytes.size(); ++Offset)
		{
			Durin::uint64 CandidateSize = 0;
			std::memcpy(&CandidateSize, Bytes.data() + Offset, sizeof(CandidateSize));
			if (CandidateSize == NameSize
				&& std::memcmp(Bytes.data() + Offset + sizeof(CandidateSize), Name.data(), Name.size()) == 0)
			{
				return true;
			}
		}
		return false;
	}

	auto ReplaceAll(std::string& Text, std::string_view From, std::string_view To) -> void
	{
		size_t Offset = 0;
		while ((Offset = Text.find(From, Offset)) != std::string::npos)
		{
			Text.replace(Offset, From.size(), To);
			Offset += To.size();
		}
	}

		auto WriteStaticMeshSlotVariant(
		const std::filesystem::path& Path,
		std::string_view MaterialDeclarations,
		std::optional<std::pair<std::string_view, std::string_view>> PrimitiveReplacement = std::nullopt,
		bool bReplaceLastOnly = false,
		std::optional<Durin::uint32> AppendedMaterialIndex = std::nullopt,
		bool bSwapPrimitiveMaterialIndices = false) -> void
	{
		std::ifstream Input(std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf");
		ASSERT_TRUE(Input.is_open());
		std::string Text((std::istreambuf_iterator<char>(Input)), std::istreambuf_iterator<char>());
		const size_t MaterialsBegin = Text.find("\"materials\": [");
		const size_t MaterialsEnd = Text.find("],", MaterialsBegin);
		ASSERT_NE(MaterialsBegin, std::string::npos);
		ASSERT_NE(MaterialsEnd, std::string::npos);
		const size_t ContentBegin = Text.find('[', MaterialsBegin) + 1;
		Text.replace(ContentBegin, MaterialsEnd - ContentBegin, MaterialDeclarations);
		if (PrimitiveReplacement)
		{
			if (bReplaceLastOnly)
			{
				const size_t Offset = Text.rfind(PrimitiveReplacement->first);
				ASSERT_NE(Offset, std::string::npos);
				Text.replace(Offset, PrimitiveReplacement->first.size(), PrimitiveReplacement->second);
			}
			else
			{
				ReplaceAll(Text, PrimitiveReplacement->first, PrimitiveReplacement->second);
			}
		}
		if (AppendedMaterialIndex)
		{
			const size_t PrimitiveBegin = Text.find("{ \"attributes\"");
			const size_t PrimitiveEnd = Text.find('\n', PrimitiveBegin);
			ASSERT_NE(PrimitiveBegin, std::string::npos);
			ASSERT_NE(PrimitiveEnd, std::string::npos);
			std::string Primitive = Text.substr(PrimitiveBegin, PrimitiveEnd - PrimitiveBegin);
			if (!Primitive.empty() && Primitive.back() == ',') Primitive.pop_back();
			const size_t MaterialOffset = Primitive.rfind("\"material\": ");
			ASSERT_NE(MaterialOffset, std::string::npos);
			const size_t ValueOffset = MaterialOffset + std::string_view("\"material\": ").size();
			Primitive.replace(ValueOffset, 1, std::to_string(*AppendedMaterialIndex));
			const size_t PrimitivesEnd = Text.find("\n      ]", PrimitiveEnd);
			ASSERT_NE(PrimitivesEnd, std::string::npos);
			Text.insert(PrimitivesEnd, ",\n        " + Primitive);
		}
		if (bSwapPrimitiveMaterialIndices)
		{
			ReplaceAll(Text, R"("material": 0)", R"("material": 2)");
			ReplaceAll(Text, R"("material": 1)", R"("material": 0)");
			ReplaceAll(Text, R"("material": 2)", R"("material": 1)");
			const size_t FirstBegin = Text.find("        { \"attributes\"");
			const size_t FirstEnd = Text.find('\n', FirstBegin);
			const size_t SecondBegin = Text.find("        { \"attributes\"", FirstEnd);
			const size_t SecondEnd = Text.find('\n', SecondBegin);
			ASSERT_NE(FirstBegin, std::string::npos);
			ASSERT_NE(FirstEnd, std::string::npos);
			ASSERT_NE(SecondBegin, std::string::npos);
			ASSERT_NE(SecondEnd, std::string::npos);
			std::string First = Text.substr(FirstBegin, FirstEnd - FirstBegin);
			std::string Second = Text.substr(SecondBegin, SecondEnd - SecondBegin);
			if (!First.empty() && First.back() == ',') First.pop_back();
			if (!Second.empty() && Second.back() == ',') Second.pop_back();
			Text.replace(FirstBegin, SecondEnd - FirstBegin, Second + ",\n" + First);
		}
		std::ofstream Output(Path, std::ios::trunc);
		ASSERT_TRUE(Output.is_open());
		Output << Text;
		ASSERT_TRUE(Output.good());
	}

	class FMaterialTestEngine final : public Durin::DEngine
	{
	public:
		FMaterialTestEngine()
			: DEngine(Durin::FObjectInitializer::Get())
		{
		}

		auto CreateTestScene() -> Durin::FScene*
		{
			auto Scene = std::make_unique<Durin::FScene>();
			Durin::FScene* Result = Scene.get();
			MainScene = std::move(Scene);
			return Result;
		}

		auto ResetTestScene() -> void { MainScene.reset(); }
		auto SetTestRendererModule(Durin::IRendererModule* InRendererModule) -> void { RendererModule = InRendererModule; }
	};

	auto WaitForRenderingThread() -> void
	{
		Durin::FRenderCommandFence Fence;
		Fence.BeginFence();
		Fence.Wait();
	}

	struct FSceneSnapshot
	{
		Durin::FStaticMeshSceneProxy* Proxy = nullptr;
		Durin::FMaterialRenderData Material;
		Durin::FMatrix Transform{1.0};
		Durin::uint64 ComponentRevision = 0;
		Durin::uint64 MaterialVersion = 0;
		Durin::uint64 ProxyCount = 0;
	};

	struct FMaterialSlotsSnapshot
	{
		Durin::FStaticMeshSceneProxy* Proxy = nullptr;
		Durin::FStaticMeshRenderData* RenderData = nullptr;
		std::vector<Durin::FMaterialRenderData> Materials;
		std::vector<Durin::uint64> MaterialVersions;
		std::vector<Durin::EMaterialRenderDirtyFlags> MaterialDirtyFlags;
		Durin::uint64 ComponentRevision = 0;
	};

	auto CaptureScene(Durin::FScene* Scene) -> FSceneSnapshot
	{
		FSceneSnapshot Snapshot;
		struct FCaptureMaterialTestSceneCommand
		{
			static constexpr const char* GetName() { return "CaptureMaterialTestScene"; }
		};
		Durin::EnqueueRenderCommand<FCaptureMaterialTestSceneCommand>([Scene, &Snapshot](Durin::FRHICommandListImmediate& CommandList) {
			Snapshot.ProxyCount = Scene->GetPrimitiveSceneProxies().size();
			if (Scene->GetPrimitiveSceneProxies().empty()) return;
			Snapshot.Proxy = dynamic_cast<Durin::FStaticMeshSceneProxy*>(Scene->GetPrimitiveSceneProxies().front());
			if (Snapshot.Proxy == nullptr) return;
			Snapshot.Material = Snapshot.Proxy->GetMaterialRenderData();
			Snapshot.Transform = Snapshot.Proxy->GetLocalToWorld();
			Snapshot.ComponentRevision = Snapshot.Proxy->GetMaterialComponentRevision();
			Snapshot.MaterialVersion = Snapshot.Proxy->GetMaterialVersion();
		});
		WaitForRenderingThread();
		return Snapshot;
	}

	auto CaptureMaterialSlots(Durin::FScene* Scene) -> FMaterialSlotsSnapshot
	{
		FMaterialSlotsSnapshot Snapshot;
		struct FCaptureMaterialSlotsCommand
		{
			static constexpr const char* GetName() { return "CaptureMaterialSlots"; }
		};
		Durin::EnqueueRenderCommand<FCaptureMaterialSlotsCommand>([Scene, &Snapshot](Durin::FRHICommandListImmediate&) {
			if (Scene->GetPrimitiveSceneProxies().empty()) return;
			Snapshot.Proxy = dynamic_cast<Durin::FStaticMeshSceneProxy*>(Scene->GetPrimitiveSceneProxies().front());
			if (Snapshot.Proxy == nullptr) return;
			Snapshot.RenderData = Snapshot.Proxy->GetRenderData();
			Snapshot.ComponentRevision = Snapshot.Proxy->GetMaterialComponentRevision();
			for (Durin::uint32 SlotIndex = 0; SlotIndex < Snapshot.Proxy->GetNumMaterials(); ++SlotIndex)
			{
				Snapshot.Materials.push_back(Snapshot.Proxy->GetMaterialRenderData(SlotIndex));
				Snapshot.MaterialVersions.push_back(Snapshot.Proxy->GetMaterialVersion(SlotIndex));
				Snapshot.MaterialDirtyFlags.push_back(Snapshot.Proxy->GetLastMaterialDirtyFlags(SlotIndex));
			}
		});
		WaitForRenderingThread();
		return Snapshot;
	}

	class FRenderSceneHarness
	{
	public:
		FRenderSceneHarness()
		{
			InitializeDObjectSystem();
			Durin::InitRenderingThread();
			Scene = Engine.CreateTestScene();
			Durin::GEngine = &Engine;
		}

		~FRenderSceneHarness() { Shutdown(); }

		auto Shutdown() -> void
		{
			if (!bActive) return;
			if (Scene != nullptr)
			{
				Scene->Release();
				WaitForRenderingThread();
				Engine.ResetTestScene();
				Scene = nullptr;
			}
			Durin::GEngine = nullptr;
			Durin::ShutdownRenderingThread();
			bActive = false;
		}

		FMaterialTestEngine Engine;
		Durin::FScene* Scene = nullptr;
		bool bActive = true;
	};

	class FMaterialPreviewHarness
	{
	public:
		FMaterialPreviewHarness()
		{
			InitializeDObjectSystem();
			Durin::InitRenderingThread();
			Engine.SetTestRendererModule(&RendererModule);
			Durin::GEngine = &Engine;
		}

		~FMaterialPreviewHarness()
		{
			Durin::GEngine = nullptr;
			WaitForRenderingThread();
			Durin::ShutdownRenderingThread();
		}

		FMaterialTestEngine Engine;
		Durin::FRendererModule RendererModule;
	};

	auto ExpectColorNear(const Durin::FVector4f& Actual, const Durin::FVector4f& Expected) -> void
	{
		EXPECT_NEAR(Actual.r, Expected.r, 1.e-6f);
		EXPECT_NEAR(Actual.g, Expected.g, 1.e-6f);
		EXPECT_NEAR(Actual.b, Expected.b, 1.e-6f);
		EXPECT_NEAR(Actual.a, Expected.a, 1.e-6f);
	}

	auto FindObjectByName(std::string_view Name) -> Durin::DObject*
	{
		const auto& Objects = Durin::GDObjectArray.GetAll();
		const auto It = std::ranges::find_if(Objects, [Name](const Durin::DObject* Object) {
			return Object && Object->GetName() == Name;
		});
		return It == Objects.end() ? nullptr : *It;
	}

	auto AddDebugMaterialSlot(Durin::DStaticMesh* Mesh, std::string_view Name) -> Durin::FGuid
	{
		auto* Slots = static_cast<Durin::FArrayProperty*>(Mesh->GetClass()->FindPropertyByName("MaterialSlots"));
		EXPECT_NE(Slots, nullptr);
		const Durin::uint64 Index = Slots->Num(Mesh);
		Slots->Resize(Mesh, Index + 1);
		auto* Slot = static_cast<Durin::FStaticMeshMaterialSlotDefinition*>(Slots->GetMutableElementPtr(Mesh, Index));
		Slot->SlotId = Durin::FGuid::NewGuid();
		Slot->Name = Durin::FName(Name);
		Slot->SourceName = std::string(Name);
		Slot->SourceMaterialIndex = static_cast<Durin::uint32>(Index);
		Mesh->GetRenderData()->MaterialSlots.push_back({std::string(Name), static_cast<Durin::uint32>(Index), Slot->SlotId});
		return Slot->SlotId;
	}

	auto MakeMaterialValueTarget(Durin::DMaterial* Material, const Durin::FGuid& Id, Durin::FName FieldName)
		-> std::optional<Durin::FReflectedPropertyEditTarget>
	{
		Durin::FProperty* DefinitionsProperty = Material->GetClass()->FindPropertyByName("ParameterDefinitions");
		if (!DefinitionsProperty || DefinitionsProperty->GetKind() != Durin::DurinCodeGen::EPropertyGenFlags::Array) return std::nullopt;
		auto* Definitions = static_cast<Durin::FArrayProperty*>(DefinitionsProperty);
		if (!Definitions->GetInner()
			|| Definitions->GetInner()->GetKind() != Durin::DurinCodeGen::EPropertyGenFlags::Struct) return std::nullopt;
		auto* DefinitionProperty = static_cast<Durin::FStructProperty*>(Definitions->GetInner());
		Durin::FProperty* ValueProperty = DefinitionProperty->GetStruct()->FindPropertyByName("Value");
		if (!ValueProperty || ValueProperty->GetKind() != Durin::DurinCodeGen::EPropertyGenFlags::Struct) return std::nullopt;
		auto* ValueStructProperty = static_cast<Durin::FStructProperty*>(ValueProperty);
		Durin::FProperty* Field = ValueStructProperty->GetStruct()->FindPropertyByName(FieldName);
		if (!Field) return std::nullopt;
		const std::span DefinitionsView = Material->GetParameterDefinitions();
		const auto It = std::ranges::find(DefinitionsView, Id, &Durin::FMaterialParameterDefinition::Id);
		if (It == DefinitionsView.end()) return std::nullopt;
		const Durin::uint64 Index = static_cast<Durin::uint64>(It - DefinitionsView.begin());
		void* Definition = Definitions->GetMutableElementPtr(Material, Index);
		void* Value = ValueProperty->GetValuePtr(Definition);
		return Durin::FReflectedPropertyEditTarget::ForMember(Material, Definitions)
			.ForArrayElement(Definitions->GetInner(), Index)
			.ForStructMember(ValueProperty)
			.ForStructMember(Field);
	}
}
