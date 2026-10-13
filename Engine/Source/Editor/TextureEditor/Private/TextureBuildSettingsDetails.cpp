#include "TextureBuildSettingsDetails.h"
#include "DObject/Class.h"
#include "Misc/StringHelper.h"
#include "PixelFormat.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DCompilation.h"
#include "Texture/Texture2DRenderResource.h"
#include "MonaImGui.h"
#include "Widgets/MonaImGuiPropertyTable.h"

namespace Durin::Editor::Texture
{
	namespace
	{
		auto DrawInfoRow(const char* Label, std::string_view Value) -> void
		{
			MonaImGui::PropertyEdit::BeginRow(Label, true);
			ImGui::TextUnformatted(Value.data(), Value.data() + Value.size());
			MonaImGui::PropertyEdit::EndRow(true);
		}

		auto GetEnumValueDisplayName(const char* QualifiedEnumName, uint64 Value) -> std::string
		{
			if (const DEnum* Enum = FindEnumByQualifiedName(QualifiedEnumName))
			{
				if (const FEnumValue* Record = Enum->FindValueRecordByValue(Value)) return Record->DisplayName;
			}
			return std::format("Unknown ({})", Value);
		}


		auto FormatDimensions(uint32 Width, uint32 Height) -> std::string
		{ return std::format("{} x {}", Width, Height); }

		// Declares Texture2D build controls using the host's shared property view.
		class FTextureBuildSettingsDetails final : public IDetailCustomization
		{
		public:
			auto CustomizeDetails(IDetailLayoutBuilder& Builder) -> void override
			{
				auto* Texture = Cast<DTexture2D>(Builder.GetObject());
				if (!Texture) return;
				Builder.ReplaceDefaultProperties();
				auto& Settings = Builder.EditCategory("BuildSettings", "Build Settings");
				Settings.SetSortOrder(-10);
				for (const auto& [Name, Label] : std::array{
					std::pair{"Usage", "Usage"}, std::pair{"bSRGB", "sRGB"},
					std::pair{"MaxResolution", "Max Resolution (0 = source)"},
					std::pair{"CompressionQuality", "Compression Quality"},
					std::pair{"AlphaMipMode", "Alpha Mip Mode"},
					std::pair{"AlphaCoverageThreshold", "Alpha Coverage Threshold"}})
				{
					if (auto* Property = Texture->GetClass()->FindPropertyByName(Name))
						Settings.AddProperty(Texture, Property, 0, {.Label = Label});
					else Settings.AddCustomRow(Label, [Label](auto&, const auto&) {
						DrawInfoRow(Label, "Reflection metadata unavailable"); return false;
					});
				}
				Builder.EditCategory("ResourceStatus", "Resource Status").AddCustomRow(
					"CPU Status Pixel Format Mip Count Mip Range Platform Bytes Residency GPU Availability Resource Update",
					[Texture](auto&, const auto&) {
						const FTexturePlatformData* Platform = Texture->GetPlatformData();
						if (Platform && Platform->IsValid())
						{
							uint64 TotalBytes = 0;
							for (const FTexture2DMipData& Mip : Platform->Mips) TotalBytes += Mip.Pixels.size();
							DrawInfoRow("CPU Status", "Platform data ready");
							DrawInfoRow("Pixel Format", GetPixelFormatInfo(Platform->PixelFormat).Name);
							DrawInfoRow("Mip Count", std::format("{}", Platform->Mips.size()));
							DrawInfoRow("Mip Range", std::format("{} to {}", FormatDimensions(Platform->Mips.front().Width, Platform->Mips.front().Height),
								FormatDimensions(Platform->Mips.back().Width, Platform->Mips.back().Height)));
							DrawInfoRow("Platform Bytes", StringUtils::FormatByteSize(TotalBytes));
							DrawInfoRow("Residency", "Fully resident");
						}
						else
						{
							DrawInfoRow("CPU Status", "Platform data unavailable");
						}

						DrawInfoRow("GPU Availability", Texture->HasUsableResource() ? "Available" : "Unavailable");

						DrawInfoRow("Resource Update", GetEnumValueDisplayName(
							"Durin::ETextureResourceUpdateState",
							static_cast<uint64>(Texture->GetResourceUpdateState())));

						return false;
					});
			}
		};
	}
	auto CreateTextureBuildSettingsDetails() -> std::shared_ptr<IDetailCustomization>
	{ return std::make_shared<FTextureBuildSettingsDetails>(); }
}
