#include "MonaImGui.h"

#include "Math/Transform.h"
#include "MonaImGuiBackend.h"
#include "Misc/Paths.h"
#include "Yaml/Yaml.h"

namespace Durin::MonaImGui
{
	namespace
	{
		EColorTheme GColorTheme = EColorTheme::Dark;
		float GGlobalUIScale = 1.0f;
		std::array<ImVec4, static_cast<size_t>(EUIThemeColor::Count)> GSemanticColors{};
		bool GSemanticColorsInitialized = false;

		auto SetDefaultSemanticColors() -> void
		{
			const bool bLight = GColorTheme == EColorTheme::Light;
			auto Set = [](EUIThemeColor Color, ImVec4 Value) { GSemanticColors[static_cast<size_t>(Color)] = Value; };
			Set(EUIThemeColor::Warning, bLight ? ImVec4{0.68f, 0.38f, 0.04f, 1.0f} : ImVec4{0.95f, 0.65f, 0.25f, 1.0f});
			Set(EUIThemeColor::Error, bLight ? ImVec4{0.74f, 0.16f, 0.16f, 1.0f} : ImVec4{1.0f, 0.32f, 0.32f, 1.0f});
			Set(EUIThemeColor::Info, bLight ? ImVec4{0.08f, 0.43f, 0.70f, 1.0f} : ImVec4{0.40f, 0.72f, 1.0f, 1.0f});
			Set(EUIThemeColor::Success, bLight ? ImVec4{0.12f, 0.50f, 0.22f, 1.0f} : ImVec4{0.30f, 0.78f, 0.38f, 1.0f});
			Set(EUIThemeColor::Folder, bLight ? ImVec4{0.75f, 0.48f, 0.08f, 1.0f} : ImVec4{0.91f, 0.71f, 0.27f, 1.0f});
			Set(EUIThemeColor::Asset, bLight ? ImVec4{0.08f, 0.48f, 0.72f, 1.0f} : ImVec4{0.29f, 0.68f, 0.94f, 1.0f});
			Set(EUIThemeColor::SourceFile, bLight ? ImVec4{0.42f, 0.45f, 0.52f, 1.0f} : ImVec4{0.59f, 0.61f, 0.67f, 1.0f});
			Set(EUIThemeColor::AxisX, {1.0f, 0.28f, 0.28f, 1.0f});
			Set(EUIThemeColor::AxisY, {0.28f, 0.90f, 0.38f, 1.0f});
			Set(EUIThemeColor::AxisZ, {0.31f, 0.53f, 1.0f, 1.0f});
			Set(EUIThemeColor::ViewportText, {1.0f, 1.0f, 1.0f, bLight ? 0.92f : 0.78f});
			Set(EUIThemeColor::ViewportShadow, {0.0f, 0.0f, 0.0f, 0.70f});
			Set(EUIThemeColor::ConsoleTimestamp, bLight ? ImVec4{0.42f, 0.44f, 0.48f, 1.0f} : ImVec4{0.48f, 0.50f, 0.54f, 1.0f});
			Set(EUIThemeColor::ConsoleModule, bLight ? ImVec4{0.16f, 0.42f, 0.62f, 1.0f} : ImVec4{0.55f, 0.72f, 0.88f, 1.0f});
			Set(EUIThemeColor::SelectionPrimary, {1.0f, 0.72f, 0.19f, 1.0f});
			Set(EUIThemeColor::SelectionSecondary, {0.35f, 0.67f, 1.0f, 0.86f});
			GSemanticColorsInitialized = true;
		}

		auto HexDigit(char Character) -> int32
		{
			if (Character >= '0' && Character <= '9') return Character - '0';
			if (Character >= 'a' && Character <= 'f') return Character - 'a' + 10;
			if (Character >= 'A' && Character <= 'F') return Character - 'A' + 10;
			return -1;
		}

		auto ParseHexColor(std::string_view Value, ImVec4& OutColor) -> bool
		{
			if (!Value.empty() && Value.front() == '#') Value.remove_prefix(1);
			if (Value.size() != 6 && Value.size() != 8) return false;

			uint8 Channels[4] = {0, 0, 0, 255};
			for (size_t ChannelIndex = 0; ChannelIndex < Value.size() / 2; ++ChannelIndex)
			{
				const int32 High = HexDigit(Value[ChannelIndex * 2]);
				const int32 Low = HexDigit(Value[ChannelIndex * 2 + 1]);
				if (High < 0 || Low < 0) return false;
				Channels[ChannelIndex] = static_cast<uint8>((High << 4) | Low);
			}

			constexpr float ByteToFloat = 1.0f / 255.0f;
			OutColor = {
				Channels[0] * ByteToFloat,
				Channels[1] * ByteToFloat,
				Channels[2] * ByteToFloat,
				Channels[3] * ByteToFloat
			};
			return true;
		}

		auto ApplyConfiguredColors(ImGuiStyle& Style) -> void
		{
			FYamlDocument ThemeDocument;
			const char* ConfigFileName = GColorTheme == EColorTheme::Light ? "DurinEditorTheme.Light.yaml" : "DurinEditorTheme.Dark.yaml";
			if (!ThemeDocument.LoadFromFile(FPaths::EngineDir() + "Configs/" + ConfigFileName)) return;

			const FYamlNodeView Colors = ThemeDocument.GetRootView().GetView("Colors");
			const std::array<std::pair<std::string_view, ImGuiCol>, 45> ColorBindings = {{
				{"Text", ImGuiCol_Text},
				{"TextDisabled", ImGuiCol_TextDisabled},
				{"WindowBg", ImGuiCol_WindowBg},
				{"ChildBg", ImGuiCol_ChildBg},
				{"PopupBg", ImGuiCol_PopupBg},
				{"Border", ImGuiCol_Border},
				{"BorderShadow", ImGuiCol_BorderShadow},
				{"FrameBg", ImGuiCol_FrameBg},
				{"FrameBgHovered", ImGuiCol_FrameBgHovered},
				{"FrameBgActive", ImGuiCol_FrameBgActive},
				{"TitleBg", ImGuiCol_TitleBg},
				{"TitleBgActive", ImGuiCol_TitleBgActive},
				{"TitleBgCollapsed", ImGuiCol_TitleBgCollapsed},
				{"MenuBarBg", ImGuiCol_MenuBarBg},
				{"ScrollbarBg", ImGuiCol_ScrollbarBg},
				{"ScrollbarGrab", ImGuiCol_ScrollbarGrab},
				{"ScrollbarGrabHovered", ImGuiCol_ScrollbarGrabHovered},
				{"ScrollbarGrabActive", ImGuiCol_ScrollbarGrabActive},
				{"CheckMark", ImGuiCol_CheckMark},
				{"SliderGrab", ImGuiCol_SliderGrab},
				{"SliderGrabActive", ImGuiCol_SliderGrabActive},
				{"Button", ImGuiCol_Button},
				{"ButtonHovered", ImGuiCol_ButtonHovered},
				{"ButtonActive", ImGuiCol_ButtonActive},
				{"Header", ImGuiCol_Header},
				{"HeaderHovered", ImGuiCol_HeaderHovered},
				{"HeaderActive", ImGuiCol_HeaderActive},
				{"Separator", ImGuiCol_Separator},
				{"SeparatorHovered", ImGuiCol_SeparatorHovered},
				{"SeparatorActive", ImGuiCol_SeparatorActive},
				{"ResizeGrip", ImGuiCol_ResizeGrip},
				{"ResizeGripHovered", ImGuiCol_ResizeGripHovered},
				{"ResizeGripActive", ImGuiCol_ResizeGripActive},
				{"Tab", ImGuiCol_Tab},
				{"TabHovered", ImGuiCol_TabHovered},
				{"TabSelected", ImGuiCol_TabSelected},
				{"TabDimmed", ImGuiCol_TabDimmed},
				{"TabDimmedSelected", ImGuiCol_TabDimmedSelected},
				{"DockingPreview", ImGuiCol_DockingPreview},
				{"DockingEmptyBg", ImGuiCol_DockingEmptyBg},
				{"TableHeaderBg", ImGuiCol_TableHeaderBg},
				{"TableBorderStrong", ImGuiCol_TableBorderStrong},
				{"TableBorderLight", ImGuiCol_TableBorderLight},
				{"TableRowBgAlt", ImGuiCol_TableRowBgAlt},
				{"TextSelectedBg", ImGuiCol_TextSelectedBg},
			}};

			for (const auto& [Name, ColorIndex] : ColorBindings)
			{
				ParseHexColor(Colors.GetView(Name).GetString(), Style.Colors[ColorIndex]);
			}
			ParseHexColor(Colors.GetView("NavCursor").GetString(), Style.Colors[ImGuiCol_NavCursor]);
			ParseHexColor(Colors.GetView("ModalWindowDimBg").GetString(), Style.Colors[ImGuiCol_ModalWindowDimBg]);

			const FYamlNodeView SemanticColors = ThemeDocument.GetRootView().GetView("SemanticColors");
			const std::array<std::pair<std::string_view, EUIThemeColor>, static_cast<size_t>(EUIThemeColor::Count)> SemanticBindings = {{
				{"Warning", EUIThemeColor::Warning},
				{"Error", EUIThemeColor::Error},
				{"Info", EUIThemeColor::Info},
				{"Success", EUIThemeColor::Success},
				{"Folder", EUIThemeColor::Folder},
				{"Asset", EUIThemeColor::Asset},
				{"SourceFile", EUIThemeColor::SourceFile},
				{"AxisX", EUIThemeColor::AxisX},
				{"AxisY", EUIThemeColor::AxisY},
				{"AxisZ", EUIThemeColor::AxisZ},
				{"ViewportText", EUIThemeColor::ViewportText},
				{"ViewportShadow", EUIThemeColor::ViewportShadow},
				{"ConsoleTimestamp", EUIThemeColor::ConsoleTimestamp},
				{"ConsoleModule", EUIThemeColor::ConsoleModule},
				{"SelectionPrimary", EUIThemeColor::SelectionPrimary},
				{"SelectionSecondary", EUIThemeColor::SelectionSecondary},
			}};
			uint32 InvalidSemanticColorCount = 0;
			for (const auto& [Name, Color] : SemanticBindings)
			{
				ImVec4 ParsedColor;
				if (ParseHexColor(SemanticColors.GetView(Name).GetString(), ParsedColor))
					GSemanticColors[static_cast<size_t>(Color)] = ParsedColor;
				else
					++InvalidSemanticColorCount;
			}
			if (InvalidSemanticColorCount > 0)
				DURIN_WARN("Editor theme '{}' has {} missing or invalid semantic colors; built-in defaults will be used.", ConfigFileName, InvalidSemanticColorCount);
		}

		auto MakeDurinDarkStyle() -> ImGuiStyle
		{
			SetDefaultSemanticColors();
			ImGuiStyle Style;
			if (GColorTheme == EColorTheme::Light)
				ImGui::StyleColorsLight(&Style);
			else
				ImGui::StyleColorsDark(&Style);

			Style.WindowPadding = {10.0f, 10.0f};
			Style.FramePadding = {7.0f, 4.0f};
			Style.CellPadding = {7.0f, 5.0f};
			Style.ItemSpacing = {8.0f, 6.0f};
			Style.ItemInnerSpacing = {6.0f, 4.0f};
			Style.ScrollbarSize = 13.0f;
			Style.GrabMinSize = 10.0f;
			Style.WindowBorderSize = 1.0f;
			Style.ChildBorderSize = 1.0f;
			Style.PopupBorderSize = 1.0f;
			Style.FrameBorderSize = 0.0f;
			Style.TabBorderSize = 0.0f;
			Style.DisabledAlpha = 0.4f;
			Style.WindowRounding = 5.0f;
			Style.ChildRounding = 4.0f;
			Style.FrameRounding = 4.0f;
			Style.PopupRounding = 5.0f;
			Style.ScrollbarRounding = 8.0f;
			Style.GrabRounding = 4.0f;
			Style.TabRounding = 4.0f;

			auto& Colors = Style.Colors;
			if (GColorTheme == EColorTheme::Dark)
			{
				Colors[ImGuiCol_Text] = {0.88f, 0.90f, 0.94f, 1.00f};
				Colors[ImGuiCol_TextDisabled] = {0.47f, 0.50f, 0.57f, 1.00f};
				Colors[ImGuiCol_WindowBg] = {0.075f, 0.080f, 0.100f, 1.00f};
				Colors[ImGuiCol_ChildBg] = {0.075f, 0.080f, 0.100f, 0.00f};
				Colors[ImGuiCol_PopupBg] = {0.105f, 0.115f, 0.145f, 0.98f};
				Colors[ImGuiCol_Border] = {0.22f, 0.24f, 0.30f, 0.75f};
				Colors[ImGuiCol_BorderShadow] = {0.00f, 0.00f, 0.00f, 0.00f};
				Colors[ImGuiCol_FrameBg] = {0.135f, 0.145f, 0.180f, 1.00f};
				Colors[ImGuiCol_FrameBgHovered] = {0.18f, 0.20f, 0.25f, 1.00f};
				Colors[ImGuiCol_FrameBgActive] = {0.20f, 0.22f, 0.29f, 1.00f};
				Colors[ImGuiCol_TitleBg] = {0.070f, 0.075f, 0.095f, 1.00f};
				Colors[ImGuiCol_TitleBgActive] = {0.095f, 0.105f, 0.135f, 1.00f};
				Colors[ImGuiCol_TitleBgCollapsed] = {0.070f, 0.075f, 0.095f, 0.80f};
				Colors[ImGuiCol_MenuBarBg] = {0.095f, 0.100f, 0.125f, 1.00f};
				Colors[ImGuiCol_ScrollbarBg] = {0.060f, 0.065f, 0.080f, 0.65f};
				Colors[ImGuiCol_ScrollbarGrab] = {0.24f, 0.26f, 0.32f, 1.00f};
				Colors[ImGuiCol_ScrollbarGrabHovered] = {0.32f, 0.35f, 0.43f, 1.00f};
				Colors[ImGuiCol_ScrollbarGrabActive] = {0.38f, 0.42f, 0.52f, 1.00f};
				Colors[ImGuiCol_CheckMark] = {0.20f, 0.72f, 0.96f, 1.00f};
				Colors[ImGuiCol_SliderGrab] = {0.20f, 0.63f, 0.90f, 1.00f};
				Colors[ImGuiCol_SliderGrabActive] = {0.42f, 0.55f, 0.96f, 1.00f};
				Colors[ImGuiCol_Button] = {0.16f, 0.18f, 0.23f, 1.00f};
				Colors[ImGuiCol_ButtonHovered] = {0.20f, 0.38f, 0.56f, 1.00f};
				Colors[ImGuiCol_ButtonActive] = {0.25f, 0.42f, 0.68f, 1.00f};
				Colors[ImGuiCol_Header] = {0.15f, 0.18f, 0.24f, 1.00f};
				Colors[ImGuiCol_HeaderHovered] = {0.19f, 0.34f, 0.51f, 1.00f};
				Colors[ImGuiCol_HeaderActive] = {0.23f, 0.40f, 0.65f, 1.00f};
				Colors[ImGuiCol_Separator] = {0.22f, 0.24f, 0.30f, 0.75f};
				Colors[ImGuiCol_SeparatorHovered] = {0.24f, 0.60f, 0.90f, 0.85f};
				Colors[ImGuiCol_SeparatorActive] = {0.34f, 0.50f, 0.94f, 1.00f};
				Colors[ImGuiCol_ResizeGrip] = {0.20f, 0.60f, 0.90f, 0.18f};
				Colors[ImGuiCol_ResizeGripHovered] = {0.24f, 0.65f, 0.94f, 0.65f};
				Colors[ImGuiCol_ResizeGripActive] = {0.42f, 0.50f, 0.96f, 0.90f};
				Colors[ImGuiCol_Tab] = {0.105f, 0.115f, 0.145f, 1.00f};
				Colors[ImGuiCol_TabHovered] = {0.18f, 0.36f, 0.56f, 1.00f};
				Colors[ImGuiCol_TabSelected] = {0.16f, 0.25f, 0.39f, 1.00f};
				Colors[ImGuiCol_TabDimmed] = {0.080f, 0.085f, 0.105f, 1.00f};
				Colors[ImGuiCol_TabDimmedSelected] = {0.12f, 0.16f, 0.23f, 1.00f};
				Colors[ImGuiCol_DockingPreview] = {0.24f, 0.64f, 0.94f, 0.55f};
				Colors[ImGuiCol_DockingEmptyBg] = {0.055f, 0.060f, 0.075f, 1.00f};
				Colors[ImGuiCol_TableHeaderBg] = {0.12f, 0.13f, 0.16f, 1.00f};
				Colors[ImGuiCol_TableBorderStrong] = {0.24f, 0.26f, 0.32f, 1.00f};
				Colors[ImGuiCol_TableBorderLight] = {0.17f, 0.18f, 0.23f, 1.00f};
				Colors[ImGuiCol_TableRowBgAlt] = {1.00f, 1.00f, 1.00f, 0.025f};
				Colors[ImGuiCol_TextSelectedBg] = {0.22f, 0.48f, 0.82f, 0.45f};
				Colors[ImGuiCol_NavCursor] = {0.38f, 0.57f, 0.98f, 1.00f};
				Colors[ImGuiCol_ModalWindowDimBg] = {0.02f, 0.025f, 0.04f, 0.72f};
			}
			ApplyConfiguredColors(Style);

			return Style;
		}
	} // namespace

	auto DrawTexture(const FRHITexture* Texture, const FVector2f& Size) -> void
	{
		ImGui::Image(reinterpret_cast<ImTextureID>(Texture), {Size.x, Size.y});
	}

	auto BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void
	{
		FMonaImGuiBackend::Get().BindMainViewportToWindow(Window);
	}

	auto SetGlobalUIScale(float Scale) -> void
	{
		Scale = std::clamp(Scale, 0.75f, 2.0f);
		GGlobalUIScale = Scale;
		ImGuiStyle& Style = ImGui::GetStyle();
		Style = MakeDurinDarkStyle();
		Style.ScaleAllSizes(Scale);
		Style.FontScaleMain = Scale;
	}

	auto GetGlobalUIScale() -> float
	{
		return GGlobalUIScale;
	}

	auto ScaleUI(float BaseValue) -> float
	{
		return BaseValue * GGlobalUIScale;
	}

	auto GetUIStyleMetrics() -> FUIStyleMetrics
	{
		FUIStyleMetrics Metrics;
		Metrics.SpacingXS = ScaleUI(Metrics.SpacingXS);
		Metrics.SpacingS = ScaleUI(Metrics.SpacingS);
		Metrics.SpacingM = ScaleUI(Metrics.SpacingM);
		Metrics.SpacingL = ScaleUI(Metrics.SpacingL);
		Metrics.SplitterThickness = ScaleUI(Metrics.SplitterThickness);
		Metrics.StandardPopupWidth = ScaleUI(Metrics.StandardPopupWidth);
		Metrics.WidePopupWidth = ScaleUI(Metrics.WidePopupWidth);
		Metrics.CompactButtonWidth = ScaleUI(Metrics.CompactButtonWidth);
		Metrics.StandardButtonWidth = ScaleUI(Metrics.StandardButtonWidth);
		return Metrics;
	}

	auto SetColorTheme(EColorTheme Theme) -> void
	{
		GColorTheme = Theme;
		SetGlobalUIScale(GGlobalUIScale);
	}

	auto GetColorTheme() -> EColorTheme
	{
		return GColorTheme;
	}

	auto GetThemeColor(EUIThemeColor Color) -> const ImVec4&
	{
		if (!GSemanticColorsInitialized) SetDefaultSemanticColors();
		const size_t Index = static_cast<size_t>(Color);
		return GSemanticColors[Index < GSemanticColors.size() ? Index : 0];
	}

	auto GetThemeColorU32(EUIThemeColor Color) -> ImU32
	{
		return ImGui::GetColorU32(GetThemeColor(Color));
	}
} // namespace Durin::MonaImGui
