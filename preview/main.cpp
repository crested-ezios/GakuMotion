// =============================================================================
// preview/main.cpp — GakuMotion 控制面板 Win32 ImGui 独立预览目标
// =============================================================================
// 本程序作为 Visual Studio 的独立 Startup Target (按 F5 直接运行)。
// 完全复用与游戏内一致的 GkGuiOverlay 及各 Tab 渲染逻辑，以最薄 Mock State
// 提供展示数据与交互响应，不依赖 Unity、IL2CPP、GakuLoader 或游戏环境。
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <tchar.h>

#define GKMOTION_ENABLE_IMGUI 1
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>

#include "GkMockState.hpp"
#include "../src/GkGuiOverlay.hpp"
#include "../src/GkGmlClient.hpp"
#include "GakuPlugin.h"

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
	ID3D11Device* g_pd3dDevice = nullptr;
	ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
	IDXGISwapChain* g_pSwapChain = nullptr;
	ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
	bool g_hostedMode = false;
	GakuUiTheme g_previewTheme = GAKU_UI_THEME_DARK;

	GakuStatus PreviewGetAppearance(uint32_t size, GakuUiAppearanceV1* out)
	{
		if (!out || size < sizeof(GakuUiAppearanceV1)) return GAKU_E_INVALID_ARGUMENT;
		out->apiVersion = 1;
		out->structSize = sizeof(GakuUiAppearanceV1);
		out->theme = g_previewTheme;
		out->uiScale = 1.0f;
		return GAKU_OK;
	}

	void SetupDarkTheme(ImGuiStyle& style)
	{
		ImGui::StyleColorsDark(&style);
		ImVec4* colors = style.Colors;
		colors[ImGuiCol_Text]                  = ImVec4(0.92f, 0.93f, 0.95f, 1.00f);
		colors[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.53f, 0.58f, 1.00f);
		colors[ImGuiCol_WindowBg]              = ImVec4(0.12f, 0.13f, 0.15f, 1.00f);
		colors[ImGuiCol_ChildBg]               = ImVec4(0.14f, 0.15f, 0.18f, 1.00f);
		colors[ImGuiCol_PopupBg]               = ImVec4(0.13f, 0.14f, 0.17f, 0.98f);
		colors[ImGuiCol_Border]                = ImVec4(0.24f, 0.26f, 0.30f, 0.70f);
		colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
		colors[ImGuiCol_FrameBg]               = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
		colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.24f, 0.27f, 0.33f, 1.00f);
		colors[ImGuiCol_FrameBgActive]         = ImVec4(0.28f, 0.32f, 0.38f, 1.00f);
		colors[ImGuiCol_TitleBg]               = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
		colors[ImGuiCol_TitleBgActive]         = ImVec4(0.14f, 0.16f, 0.20f, 1.00f);
		colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.10f, 0.11f, 0.13f, 0.75f);
		colors[ImGuiCol_MenuBarBg]             = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
		colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.10f, 0.11f, 0.13f, 0.50f);
		colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.26f, 0.29f, 0.35f, 1.00f);
		colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.32f, 0.36f, 0.44f, 1.00f);
		colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.38f, 0.43f, 0.52f, 1.00f);
		colors[ImGuiCol_CheckMark]             = ImVec4(0.35f, 0.65f, 0.95f, 1.00f);
		colors[ImGuiCol_SliderGrab]            = ImVec4(0.30f, 0.55f, 0.85f, 1.00f);
		colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.40f, 0.68f, 0.98f, 1.00f);
		colors[ImGuiCol_Button]                = ImVec4(0.20f, 0.23f, 0.28f, 1.00f);
		colors[ImGuiCol_ButtonHovered]         = ImVec4(0.28f, 0.33f, 0.40f, 1.00f);
		colors[ImGuiCol_ButtonActive]          = ImVec4(0.34f, 0.40f, 0.50f, 1.00f);
		colors[ImGuiCol_Header]                = ImVec4(0.20f, 0.24f, 0.30f, 1.00f);
		colors[ImGuiCol_HeaderHovered]         = ImVec4(0.26f, 0.32f, 0.40f, 1.00f);
		colors[ImGuiCol_HeaderActive]          = ImVec4(0.32f, 0.38f, 0.48f, 1.00f);
		colors[ImGuiCol_Separator]             = ImVec4(0.24f, 0.26f, 0.30f, 0.70f);
		colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.35f, 0.45f, 0.60f, 1.00f);
		colors[ImGuiCol_SeparatorActive]       = ImVec4(0.45f, 0.58f, 0.75f, 1.00f);
		colors[ImGuiCol_ResizeGrip]            = ImVec4(0.25f, 0.30f, 0.38f, 0.50f);
		colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.35f, 0.42f, 0.55f, 0.70f);
		colors[ImGuiCol_ResizeGripActive]      = ImVec4(0.45f, 0.55f, 0.70f, 0.90f);
		colors[ImGuiCol_Tab]                   = ImVec4(0.16f, 0.18f, 0.22f, 1.00f);
		colors[ImGuiCol_TabHovered]            = ImVec4(0.26f, 0.32f, 0.40f, 1.00f);
		colors[ImGuiCol_TabActive]             = ImVec4(0.22f, 0.26f, 0.33f, 1.00f);
		colors[ImGuiCol_TabUnfocused]          = ImVec4(0.14f, 0.15f, 0.18f, 1.00f);
		colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
		colors[ImGuiCol_TableHeaderBg]         = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
		colors[ImGuiCol_TableBorderStrong]     = ImVec4(0.26f, 0.29f, 0.35f, 1.00f);
		colors[ImGuiCol_TableBorderLight]      = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
		colors[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
		colors[ImGuiCol_TableRowBgAlt]         = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);
		colors[ImGuiCol_TextSelectedBg]        = ImVec4(0.25f, 0.45f, 0.75f, 0.40f);
		colors[ImGuiCol_NavHighlight]          = ImVec4(0.35f, 0.65f, 0.95f, 1.00f);

		style.WindowRounding    = 5.0f;
		style.FrameRounding     = 4.0f;
		style.PopupRounding     = 4.0f;
		style.ScrollbarRounding = 4.0f;
		style.GrabRounding      = 3.0f;
		style.TabRounding       = 4.0f;
		style.WindowBorderSize  = 1.0f;
		style.FrameBorderSize   = 0.0f;
		style.PopupBorderSize   = 1.0f;
		style.WindowPadding     = ImVec2(10.0f, 10.0f);
		style.FramePadding      = ImVec2(6.0f, 4.0f);
		style.ItemSpacing       = ImVec2(8.0f, 6.0f);
		style.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
		style.ScrollbarSize     = 14.0f;
		style.GrabMinSize       = 10.0f;
	}

	void SetupSapphireTheme(ImGuiStyle& style)
	{
		ImGui::StyleColorsDark(&style);
		ImVec4* colors = style.Colors;
		colors[ImGuiCol_Text]                  = ImVec4(0.92f, 0.95f, 0.98f, 1.00f);
		colors[ImGuiCol_TextDisabled]          = ImVec4(0.48f, 0.55f, 0.68f, 1.00f);
		colors[ImGuiCol_WindowBg]              = ImVec4(0.08f, 0.10f, 0.15f, 1.00f);
		colors[ImGuiCol_ChildBg]               = ImVec4(0.10f, 0.12f, 0.18f, 1.00f);
		colors[ImGuiCol_PopupBg]               = ImVec4(0.09f, 0.11f, 0.17f, 0.98f);
		colors[ImGuiCol_Border]                = ImVec4(0.20f, 0.28f, 0.42f, 0.65f);
		colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
		colors[ImGuiCol_FrameBg]               = ImVec4(0.13f, 0.17f, 0.26f, 1.00f);
		colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.18f, 0.24f, 0.36f, 1.00f);
		colors[ImGuiCol_FrameBgActive]         = ImVec4(0.22f, 0.30f, 0.46f, 1.00f);
		colors[ImGuiCol_TitleBg]               = ImVec4(0.07f, 0.09f, 0.14f, 1.00f);
		colors[ImGuiCol_TitleBgActive]         = ImVec4(0.12f, 0.16f, 0.26f, 1.00f);
		colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.07f, 0.09f, 0.14f, 0.75f);
		colors[ImGuiCol_MenuBarBg]             = ImVec4(0.09f, 0.11f, 0.17f, 1.00f);
		colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.07f, 0.09f, 0.14f, 0.50f);
		colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.18f, 0.26f, 0.42f, 1.00f);
		colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.24f, 0.34f, 0.54f, 1.00f);
		colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.30f, 0.42f, 0.65f, 1.00f);
		colors[ImGuiCol_CheckMark]             = ImVec4(0.38f, 0.72f, 1.00f, 1.00f);
		colors[ImGuiCol_SliderGrab]            = ImVec4(0.30f, 0.58f, 0.90f, 1.00f);
		colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.42f, 0.72f, 1.00f, 1.00f);
		colors[ImGuiCol_Button]                = ImVec4(0.16f, 0.24f, 0.38f, 1.00f);
		colors[ImGuiCol_ButtonHovered]         = ImVec4(0.22f, 0.34f, 0.54f, 1.00f);
		colors[ImGuiCol_ButtonActive]          = ImVec4(0.28f, 0.44f, 0.68f, 1.00f);
		colors[ImGuiCol_Header]                = ImVec4(0.16f, 0.24f, 0.38f, 1.00f);
		colors[ImGuiCol_HeaderHovered]         = ImVec4(0.22f, 0.34f, 0.52f, 1.00f);
		colors[ImGuiCol_HeaderActive]          = ImVec4(0.28f, 0.42f, 0.64f, 1.00f);
		colors[ImGuiCol_Separator]             = ImVec4(0.20f, 0.28f, 0.42f, 0.65f);
		colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.30f, 0.45f, 0.68f, 1.00f);
		colors[ImGuiCol_SeparatorActive]       = ImVec4(0.40f, 0.60f, 0.88f, 1.00f);
		colors[ImGuiCol_ResizeGrip]            = ImVec4(0.20f, 0.32f, 0.50f, 0.50f);
		colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.28f, 0.44f, 0.68f, 0.70f);
		colors[ImGuiCol_ResizeGripActive]      = ImVec4(0.36f, 0.56f, 0.85f, 0.90f);
		colors[ImGuiCol_Tab]                   = ImVec4(0.13f, 0.18f, 0.28f, 1.00f);
		colors[ImGuiCol_TabHovered]            = ImVec4(0.22f, 0.34f, 0.52f, 1.00f);
		colors[ImGuiCol_TabActive]             = ImVec4(0.18f, 0.27f, 0.44f, 1.00f);
		colors[ImGuiCol_TabUnfocused]          = ImVec4(0.10f, 0.13f, 0.20f, 1.00f);
		colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.14f, 0.20f, 0.32f, 1.00f);
		colors[ImGuiCol_TableHeaderBg]         = ImVec4(0.13f, 0.18f, 0.28f, 1.00f);
		colors[ImGuiCol_TableBorderStrong]     = ImVec4(0.22f, 0.32f, 0.48f, 1.00f);
		colors[ImGuiCol_TableBorderLight]      = ImVec4(0.16f, 0.22f, 0.34f, 1.00f);
		colors[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
		colors[ImGuiCol_TableRowBgAlt]         = ImVec4(0.18f, 0.28f, 0.48f, 0.10f);
		colors[ImGuiCol_TextSelectedBg]        = ImVec4(0.25f, 0.45f, 0.75f, 0.45f);
		colors[ImGuiCol_NavHighlight]          = ImVec4(0.38f, 0.72f, 1.00f, 1.00f);

		style.WindowRounding    = 5.0f;
		style.FrameRounding     = 4.0f;
		style.PopupRounding     = 4.0f;
		style.ScrollbarRounding = 4.0f;
		style.GrabRounding      = 3.0f;
		style.TabRounding       = 4.0f;
		style.WindowBorderSize  = 1.0f;
		style.FrameBorderSize   = 0.0f;
		style.PopupBorderSize   = 1.0f;
		style.WindowPadding     = ImVec2(10.0f, 10.0f);
		style.FramePadding      = ImVec2(6.0f, 4.0f);
		style.ItemSpacing       = ImVec2(8.0f, 6.0f);
		style.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
		style.ScrollbarSize     = 14.0f;
		style.GrabMinSize       = 10.0f;
	}

	void SetupPaperAndInkTheme(ImGuiStyle& style)
	{
		ImGui::StyleColorsLight(&style);
		ImVec4* colors = style.Colors;
		colors[ImGuiCol_Text]                  = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
		colors[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.50f, 0.52f, 1.00f);
		colors[ImGuiCol_WindowBg]              = ImVec4(0.95f, 0.95f, 0.94f, 1.00f);
		colors[ImGuiCol_ChildBg]               = ImVec4(0.91f, 0.91f, 0.89f, 1.00f);
		colors[ImGuiCol_PopupBg]               = ImVec4(0.97f, 0.97f, 0.96f, 0.98f);
		colors[ImGuiCol_Border]                = ImVec4(0.72f, 0.72f, 0.70f, 0.85f);
		colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
		colors[ImGuiCol_FrameBg]               = ImVec4(0.86f, 0.86f, 0.84f, 1.00f);
		colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.80f, 0.80f, 0.78f, 1.00f);
		colors[ImGuiCol_FrameBgActive]         = ImVec4(0.74f, 0.74f, 0.72f, 1.00f);
		colors[ImGuiCol_TitleBg]               = ImVec4(0.90f, 0.90f, 0.88f, 1.00f);
		colors[ImGuiCol_TitleBgActive]         = ImVec4(0.84f, 0.84f, 0.82f, 1.00f);
		colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.90f, 0.90f, 0.88f, 0.75f);
		colors[ImGuiCol_MenuBarBg]             = ImVec4(0.90f, 0.90f, 0.88f, 1.00f);
		colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.91f, 0.91f, 0.89f, 0.50f);
		colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.74f, 0.74f, 0.72f, 1.00f);
		colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.62f, 0.62f, 0.60f, 1.00f);
		colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.48f, 0.48f, 0.46f, 1.00f);
		colors[ImGuiCol_CheckMark]             = ImVec4(0.18f, 0.36f, 0.58f, 1.00f);
		colors[ImGuiCol_SliderGrab]            = ImVec4(0.38f, 0.46f, 0.58f, 1.00f);
		colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.24f, 0.34f, 0.48f, 1.00f);
		colors[ImGuiCol_Button]                = ImVec4(0.83f, 0.83f, 0.81f, 1.00f);
		colors[ImGuiCol_ButtonHovered]         = ImVec4(0.76f, 0.76f, 0.74f, 1.00f);
		colors[ImGuiCol_ButtonActive]          = ImVec4(0.68f, 0.68f, 0.66f, 1.00f);
		colors[ImGuiCol_Header]                = ImVec4(0.83f, 0.84f, 0.86f, 1.00f);
		colors[ImGuiCol_HeaderHovered]         = ImVec4(0.75f, 0.77f, 0.81f, 1.00f);
		colors[ImGuiCol_HeaderActive]          = ImVec4(0.67f, 0.70f, 0.76f, 1.00f);
		colors[ImGuiCol_Separator]             = ImVec4(0.72f, 0.72f, 0.70f, 0.85f);
		colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.50f, 0.52f, 0.56f, 1.00f);
		colors[ImGuiCol_SeparatorActive]       = ImVec4(0.32f, 0.34f, 0.38f, 1.00f);
		colors[ImGuiCol_ResizeGrip]            = ImVec4(0.65f, 0.65f, 0.63f, 0.50f);
		colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.50f, 0.50f, 0.48f, 0.70f);
		colors[ImGuiCol_ResizeGripActive]      = ImVec4(0.35f, 0.35f, 0.33f, 0.90f);
		colors[ImGuiCol_Tab]                   = ImVec4(0.85f, 0.85f, 0.83f, 1.00f);
		colors[ImGuiCol_TabHovered]            = ImVec4(0.77f, 0.79f, 0.83f, 1.00f);
		colors[ImGuiCol_TabActive]             = ImVec4(0.95f, 0.95f, 0.94f, 1.00f);
		colors[ImGuiCol_TabUnfocused]          = ImVec4(0.87f, 0.87f, 0.85f, 1.00f);
		colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.91f, 0.91f, 0.89f, 1.00f);
		colors[ImGuiCol_TableHeaderBg]         = ImVec4(0.85f, 0.85f, 0.84f, 1.00f);
		colors[ImGuiCol_TableBorderStrong]     = ImVec4(0.68f, 0.68f, 0.66f, 1.00f);
		colors[ImGuiCol_TableBorderLight]      = ImVec4(0.78f, 0.78f, 0.76f, 1.00f);
		colors[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
		colors[ImGuiCol_TableRowBgAlt]         = ImVec4(0.00f, 0.00f, 0.00f, 0.04f);
		colors[ImGuiCol_TextSelectedBg]        = ImVec4(0.20f, 0.40f, 0.70f, 0.30f);
		colors[ImGuiCol_NavHighlight]          = ImVec4(0.18f, 0.36f, 0.58f, 1.00f);

		style.WindowRounding    = 5.0f;
		style.FrameRounding     = 4.0f;
		style.PopupRounding     = 4.0f;
		style.ScrollbarRounding = 4.0f;
		style.GrabRounding      = 3.0f;
		style.TabRounding       = 4.0f;
		style.WindowBorderSize  = 1.0f;
		style.FrameBorderSize   = 1.0f;
		style.PopupBorderSize   = 1.0f;
		style.WindowPadding     = ImVec2(10.0f, 10.0f);
		style.FramePadding      = ImVec2(6.0f, 4.0f);
		style.ItemSpacing       = ImVec2(8.0f, 6.0f);
		style.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
		style.ScrollbarSize     = 14.0f;
		style.GrabMinSize       = 10.0f;
	}

	void ApplyPreviewTheme(GakuUiTheme theme)
	{
		g_previewTheme = theme;
		ImGuiStyle style;
		switch (theme)
		{
		case GAKU_UI_THEME_SAPPHIRE:
			SetupSapphireTheme(style);
			break;
		case GAKU_UI_THEME_PAPER_AND_INK:
			SetupPaperAndInkTheme(style);
			break;
		case GAKU_UI_THEME_DARK:
		default:
			SetupDarkTheme(style);
			break;
		}
		ImGui::GetStyle() = style;
	}

	bool CreateDeviceD3D(HWND hWnd)
	{
		DXGI_SWAP_CHAIN_DESC sd;
		ZeroMemory(&sd, sizeof(sd));
		sd.BufferCount = 2;
		sd.BufferDesc.Width = 0;
		sd.BufferDesc.Height = 0;
		sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.BufferDesc.RefreshRate.Numerator = 60;
		sd.BufferDesc.RefreshRate.Denominator = 1;
		sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.OutputWindow = hWnd;
		sd.SampleDesc.Count = 1;
		sd.SampleDesc.Quality = 0;
		sd.Windowed = TRUE;
		sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

		UINT createDeviceFlags = 0;
		D3D_FEATURE_LEVEL featureLevel;
		const D3D_FEATURE_LEVEL featureLevelArray[2] = {
			D3D_FEATURE_LEVEL_11_0,
			D3D_FEATURE_LEVEL_10_0,
		};
		HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
		                                           createDeviceFlags, featureLevelArray, 2,
		                                           D3D11_SDK_VERSION, &sd, &g_pSwapChain,
		                                           &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
		if (FAILED(hr)) return false;

		ID3D11Texture2D* pBackBuffer = nullptr;
		g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
		if (!pBackBuffer) return false;
		g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
		pBackBuffer->Release();
		return true;
	}

	void CleanupRenderTarget()
	{
		if (g_mainRenderTargetView)
		{
			g_mainRenderTargetView->Release();
			g_mainRenderTargetView = nullptr;
		}
	}

	void CleanupDeviceD3D()
	{
		CleanupRenderTarget();
		if (g_pSwapChain)
		{
			g_pSwapChain->Release();
			g_pSwapChain = nullptr;
		}
		if (g_pd3dDeviceContext)
		{
			g_pd3dDeviceContext->Release();
			g_pd3dDeviceContext = nullptr;
		}
		if (g_pd3dDevice)
		{
			g_pd3dDevice->Release();
			g_pd3dDevice = nullptr;
		}
	}

	LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
			return true;

		switch (msg)
		{
		case WM_SIZE:
			if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED)
			{
				CleanupRenderTarget();
				g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
				ID3D11Texture2D* pBackBuffer = nullptr;
				g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
				if (pBackBuffer)
				{
					g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
					pBackBuffer->Release();
				}
			}
			return 0;
		case WM_SYSCOMMAND:
			if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
				return 0;
			break;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProcW(hWnd, msg, wParam, lParam);
	}
} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
	// 启用 DPI 感知
	ImGui_ImplWin32_EnableDpiAwareness();

	// 注册 Win32 窗口类
	WNDCLASSEXW wc = {
		sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, nullptr, nullptr, nullptr, nullptr,
		L"GakuMotionGuiPreviewClass", nullptr
	};
	RegisterClassExW(&wc);

	// 创建窗口（默认 1280x820）
	HWND hWnd = CreateWindowW(wc.lpszClassName,
	                          L"GakuMotion GUI Preview - [VS F5 Startup Target]",
	                          WS_OVERLAPPEDWINDOW, 100, 100, 1280, 820,
	                          nullptr, nullptr, wc.hInstance, nullptr);

	if (!CreateDeviceD3D(hWnd))
	{
		CleanupDeviceD3D();
		UnregisterClassW(wc.lpszClassName, wc.hInstance);
		return 1;
	}

	ShowWindow(hWnd, SW_SHOWDEFAULT);
	UpdateWindow(hWnd);

	// 初始化 Dear ImGui 上下文
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	// 初始化 GakuAppearanceService stub 使 GakuMotion 自适应颜色
	GakuMotion::host::appearance.abiVersion = GAKU_SERVICE_VERSION;
	GakuMotion::host::appearance.structSize = sizeof(GakuAppearanceService);
	GakuMotion::host::appearance.getAppearance = PreviewGetAppearance;

	// 设置主题风格
	ApplyPreviewTheme(GAKU_UI_THEME_DARK);

	// 加载系统高质量中文字体（优先微软雅黑，确保所有 Tab 中文字符高清晰度渲染）
	const wchar_t* candidateFonts[] = {
		L"C:\\Windows\\Fonts\\msyh.ttc",
		L"C:\\Windows\\Fonts\\msyh.ttf",
		L"C:\\Windows\\Fonts\\simhei.ttf",
		L"C:\\Windows\\Fonts\\simsun.ttc"
	};
	bool fontLoaded = false;
	for (const auto* path : candidateFonts)
	{
		if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
		{
			char utf8Path[MAX_PATH * 3]{};
			WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8Path, sizeof(utf8Path), nullptr, nullptr);
			ImFont* font = io.Fonts->AddFontFromFileTTF(utf8Path, 18.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
			if (font)
			{
				fontLoaded = true;
				break;
			}
		}
	}
	if (!fontLoaded)
	{
		io.Fonts->AddFontDefault();
	}

	// 初始化 ImGui 渲染与平台后端
	ImGui_ImplWin32_Init(hWnd);
	ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

	// 实例化最薄 Mock 状态及业务面板 Overlay
	GakuMotion::MockGuiState mockState;
	GakuMotion::GuiOverlay overlay;
	overlay.showWindow = true;

	// 高精度计时器初始化
	LARGE_INTEGER perfFreq, lastTime;
	QueryPerformanceFrequency(&perfFreq);
	QueryPerformanceCounter(&lastTime);

	// 主循环
	bool running = true;
	while (running)
	{
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
			if (msg.message == WM_QUIT)
			{
				running = false;
			}
		}
		if (!running) break;

		// 计算增量时间
		LARGE_INTEGER currentTime;
		QueryPerformanceCounter(&currentTime);
		float dt = static_cast<float>(currentTime.QuadPart - lastTime.QuadPart) / static_cast<float>(perfFreq.QuadPart);
		lastTime = currentTime;
		if (dt > 0.1f) dt = 0.1f; // 避免断点调试恢复后时间跳变过大

		// 更新 Mock 播放器时钟
		mockState.Update(dt);

		// 开启 ImGui 新帧
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		// 顶部工具栏
		if (ImGui::BeginMainMenuBar())
		{
			if (ImGui::BeginMenu("视图与控制 (Preview Controls)"))
			{
				ImGui::MenuItem("显示面板 (Show Panel)", nullptr, &overlay.showWindow);
				ImGui::MenuItem("宿主嵌入模式 (Hosted Panel Mode)", nullptr, &g_hostedMode);
				ImGui::Separator();
				if (ImGui::MenuItem("重置 Mock 状态 (Reset Mock State)"))
				{
					mockState.Reset();
				}
				ImGui::Separator();
				if (ImGui::MenuItem("退出预览 (Exit)", "Alt+F4"))
				{
					running = false;
				}
				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("主题 (Theme)"))
			{
				if (ImGui::MenuItem("Dark (暗色默认)", nullptr, g_previewTheme == GAKU_UI_THEME_DARK))
					ApplyPreviewTheme(GAKU_UI_THEME_DARK);
				if (ImGui::MenuItem("Sapphire (深蓝宝石)", nullptr, g_previewTheme == GAKU_UI_THEME_SAPPHIRE))
					ApplyPreviewTheme(GAKU_UI_THEME_SAPPHIRE);
				if (ImGui::MenuItem("Paper & Ink (浅色纸墨)", nullptr, g_previewTheme == GAKU_UI_THEME_PAPER_AND_INK))
					ApplyPreviewTheme(GAKU_UI_THEME_PAPER_AND_INK);
				ImGui::EndMenu();
			}

			ImGui::TextDisabled("| 提示: VS 选择 GakuMotionGuiPreview 按 F5 即可实时断点调试 | 帧率: %.1f FPS", io.Framerate);
			ImGui::EndMainMenuBar();
		}

		// 构建 GuiContext 并渲染控制面板（100% 复用游戏内相同的 Tab 绘制逻辑）
		GakuMotion::GuiContext ctx = mockState.BuildContext();
		overlay.Draw(ctx, g_hostedMode);

		// 提交 ImGui 绘制指令
		ImGui::Render();
		const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
		const float clearColor[4] = {bg.x, bg.y, bg.z, 1.0f};
		g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
		g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

		// 垂直同步呈现
		g_pSwapChain->Present(1, 0);
	}

	// 清理资源
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	CleanupDeviceD3D();
	DestroyWindow(hWnd);
	UnregisterClassW(wc.lpszClassName, wc.hInstance);

	return 0;
}
