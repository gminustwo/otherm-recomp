#include "imgui.hpp"

#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <webgpu/webgpu_cpp.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_render.h>

#include "fs_helper.hpp"
#include "input.hpp"
#include "internal.hpp"
#include "webgpu/gpu.hpp"
#include "window.hpp"

#define IMGUI_IMPL_WEBGPU_BACKEND_DAWN
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "backends/imgui_impl_wgpu.h"
#include "tracy/Tracy.hpp"

namespace aurora::imgui {
static float g_scale;
static std::string g_imguiLog{};
static bool g_useSdlRenderer = false;
// Set once ImGui::Render() has produced this frame's draw data. Interpolation encodes up to four
// ImGui passes per frame, and every one of them used to rebuild the draw lists from scratch.
static bool g_frameDataBuilt = false;

static std::vector<SDL_Texture*> g_sdlTextures;
static std::vector<wgpu::Texture> g_wgpuTextures;

// The game's own overlay menus (the runtime's hud_theme.cpp) draw text far larger than ImGui's 13 px bitmap font:
// a system sans (Bahnschrift, on Windows 10 and later, else Segoe UI) baked at three sizes, so no text is drawn at
// less than half of the size it was baked at. Fonts[0] stays ImGui's default for the developer windows.
static constexpr float kUiFontSizes[] = {24.0f, 48.0f, 96.0f};
static ImFont* g_uiFonts[3] = {};

static void add_ui_fonts() noexcept {
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->AddFontDefault();
  std::vector<std::filesystem::path> candidates;
#ifdef _WIN32
  const char* windir = std::getenv("WINDIR");
  const std::filesystem::path fonts = std::filesystem::path(windir != nullptr ? windir : "C:/Windows") / "Fonts";
  candidates = {fonts / "bahnschrift.ttf", fonts / "segoeui.ttf"};
#else
  candidates = {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf"};
#endif
  for (const auto& path : candidates) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
      continue;
    }
    const std::string file = path.string();
    for (size_t i = 0; i < std::size(kUiFontSizes); ++i) {
      ImFontConfig config;
      config.OversampleH = i == 0 ? 2 : 1;  // large text needs no sub-pixel positioning
      config.OversampleV = 1;
      g_uiFonts[i] = io.Fonts->AddFontFromFileTTF(file.c_str(), kUiFontSizes[i], &config);
    }
    break;
  }
}

ImFont* ui_font(float pixelSize) noexcept {
  for (size_t i = 0; i < std::size(kUiFontSizes); ++i) {
    if (g_uiFonts[i] != nullptr && (kUiFontSizes[i] >= pixelSize || i + 1 == std::size(kUiFontSizes))) {
      return g_uiFonts[i];
    }
  }
  return nullptr;
}

void remove_legacy_ini_file(const char* basePath) noexcept {
  if (basePath == nullptr || *basePath == '\0') {
    return;
  }

  std::error_code ec;
  std::filesystem::remove(fs_path_from_string(basePath) / "imgui.ini", ec);
}

void create_context() noexcept {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  remove_legacy_ini_file(g_config.userPath);
  remove_legacy_ini_file(g_config.cachePath);
  g_imguiLog = std::string{g_config.cachePath} + "/imgui.log";
  io.IniFilename = nullptr;
  io.LogFilename = g_imguiLog.c_str();
  add_ui_fonts();
  ImGui::LoadIniSettingsFromMemory("", 0);
  io.WantSaveIniSettings = false;
}

void initialize() noexcept {
  ZoneScoped;
  SDL_Renderer* renderer = window::get_sdl_renderer();
  ImGui_ImplSDL3_InitForSDLRenderer(window::get_sdl_window(), renderer);
  // ImGui starts/stops SDL text input from ImGui::EndFrame, which runs on the frame worker thread. On Windows that
  // sends a message to the window (ImmAssociateContext) and waits for the main thread, which is itself waiting for
  // the frame worker: closing a menu with a text field focused froze the window. So ImGui leaves text input alone.
  // It used to be switched on here for good (an earlier developer window had text fields); Other M's
  // Options has none, and with it on SDL3 opened Steam's on-screen keyboard at every start on a Steam Deck in Game
  // Mode. Off now: a text field added later gets no typed characters until text input is started on the main thread.
  ImGui::GetPlatformIO().Platform_SetImeDataFn = nullptr;
  g_useSdlRenderer = renderer != nullptr;
  if (g_useSdlRenderer) {
    ImGui_ImplSDLRenderer3_Init(renderer);
  } else {
    ImGui_ImplWGPU_InitInfo info;
    info.Device = webgpu::g_device.Get();
    info.RenderTargetFormat = static_cast<WGPUTextureFormat>(webgpu::g_graphicsConfig.surfaceConfiguration.format);
    // Interpolation records up to four ImGui passes in one command buffer, so keep three logical
    // frames of renderer resources to stop them overwriting each other's vertex/index buffers.
    info.NumFramesInFlight = 12;
    ImGui_ImplWGPU_Init(&info);
  }
}

void shutdown() noexcept {
  ZoneScoped;
  // Startup can fail before either backend initializes. A context alone does
  // not mean its renderer/platform backend owns resources to release.
  if (ImGui::GetCurrentContext() != nullptr) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.BackendRendererUserData != nullptr) {
      if (g_useSdlRenderer) {
        ImGui_ImplSDLRenderer3_Shutdown();
      } else {
        ImGui_ImplWGPU_Shutdown();
      }
    }
    if (io.BackendPlatformUserData != nullptr) {
      ImGui_ImplSDL3_Shutdown();
    }
    ImGui::DestroyContext();
  }
  for (const auto& texture : g_sdlTextures) {
    SDL_DestroyTexture(texture);
  }
  g_sdlTextures.clear();
  g_wgpuTextures.clear();
  g_useSdlRenderer = false;
  g_scale = 0.f;
  g_frameDataBuilt = false;
}

void process_event(const SDL_Event& event) noexcept {
  auto renderEvent = event;
  if (g_useSdlRenderer) {
    if (SDL_Renderer* renderer = window::get_sdl_renderer()) {
      SDL_ConvertEventToRenderCoordinates(renderer, &renderEvent);
    }
  }
  ImGui_ImplSDL3_ProcessEvent(&renderEvent);
}

bool wants_capture_event(const SDL_Event& event) noexcept {
  if (ImGui::GetCurrentContext() == nullptr) {
    return false;
  }

  const ImGuiIO& io = ImGui::GetIO();
  switch (event.type) {
  case SDL_EVENT_MOUSE_MOTION:
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
  case SDL_EVENT_MOUSE_BUTTON_UP:
  case SDL_EVENT_MOUSE_WHEEL:
  case SDL_EVENT_FINGER_DOWN:
  case SDL_EVENT_FINGER_MOTION:
  case SDL_EVENT_FINGER_UP:
  case SDL_EVENT_FINGER_CANCELED:
    return io.WantCaptureMouse;
  case SDL_EVENT_KEY_DOWN:
  case SDL_EVENT_KEY_UP:
  case SDL_EVENT_TEXT_INPUT:
    return io.WantCaptureKeyboard || io.WantTextInput;
  default:
    return false;
  }
}

void new_frame(const AuroraWindowSize& size) noexcept {
  ZoneScoped;
  ImVec2 framebufferScale{
      size.width > 0 ? static_cast<float>(size.native_fb_width) / static_cast<float>(size.width) : 1.0f,
      size.height > 0 ? static_cast<float>(size.native_fb_height) / static_cast<float>(size.height) : 1.0f,
  };
  ImVec2 displaySize{static_cast<float>(size.width), static_cast<float>(size.height)};

  if (g_useSdlRenderer) {
    if (SDL_Renderer* renderer = window::get_sdl_renderer()) {
      float renderScaleX = 1.0f;
      float renderScaleY = 1.0f;
      SDL_GetRenderScale(renderer, &renderScaleX, &renderScaleY);
      if (renderScaleX > 0.0f && renderScaleY > 0.0f &&
          (std::fabs(renderScaleX - 1.0f) > 0.0001f || std::fabs(renderScaleY - 1.0f) > 0.0001f)) {
        int outputWidth = static_cast<int>(size.native_fb_width);
        int outputHeight = static_cast<int>(size.native_fb_height);
        SDL_GetRenderOutputSize(renderer, &outputWidth, &outputHeight);
        displaySize = {
            static_cast<float>(outputWidth) / renderScaleX,
            static_cast<float>(outputHeight) / renderScaleY,
        };
        framebufferScale = {renderScaleX, renderScaleY};
      }
    }
    ImGui_ImplSDLRenderer3_NewFrame();
    g_scale = size.scale;
  } else {
    if (g_scale != size.scale) {
      if (g_scale > 0.f) {
        ImGui_ImplWGPU_CreateDeviceObjects();
      }
      g_scale = size.scale;
    }
    if (!ImGui::GetIO().Fonts->IsBuilt()) {
      ImGui_ImplWGPU_CreateDeviceObjects();
    }
    ImGui_ImplWGPU_NewFrame();
  }
  // The SDL backend reads every gamepad each frame (gamepad navigation) and reopens them after a hotplug: SDL calls
  // that wait on its joystick lock while the input thread scans devices (input_thread.cpp). Then this frame keeps the
  // backend's previous state (mouse, delta time, gamepads) rather than stall the frame worker.
  if (input::controllers_try_lock()) {
    ImGui_ImplSDL3_NewFrame();
    input::controllers_unlock();
  }

  ImGuiIO& io = ImGui::GetIO();
  io.DisplayFramebufferScale = framebufferScale;
  ImGui::GetIO().DisplaySize = displaySize;
  ImGui::NewFrame();
  g_frameDataBuilt = false;
}

void render_frame_data() noexcept {
  ZoneScoped;
  if (g_frameDataBuilt) {
    return;
  }
  ImGui::Render();
  auto* data = ImGui::GetDrawData();
  data->FramebufferScale = ImGui::GetIO().DisplayFramebufferScale;
  g_frameDataBuilt = true;
}

void render(const wgpu::RenderPassEncoder& pass) noexcept {
  ZoneScoped;
  render_frame_data();

  auto* data = ImGui::GetDrawData();
  if (g_useSdlRenderer) {
    SDL_Renderer* renderer = window::get_sdl_renderer();
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(data, renderer);
    SDL_RenderPresent(renderer);
  } else {
    pass.PushDebugGroup("Aurora: Dear Imgui");
    ImGui_ImplWGPU_RenderDrawData(data, pass.Get());
    pass.PopDebugGroup();
  }
}

ImTextureID add_texture(uint32_t width, uint32_t height, const uint8_t* data) noexcept {
  if (SDL_Renderer* renderer = window::get_sdl_renderer()) {
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
    SDL_UpdateTexture(texture, nullptr, data, width * 4);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    g_sdlTextures.push_back(texture);
    return reinterpret_cast<ImTextureID>(texture);
  }
  const wgpu::Extent3D size{
      .width = width,
      .height = height,
      .depthOrArrayLayers = 1,
  };
  const wgpu::TextureDescriptor textureDescriptor{
      .label = "imgui texture",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = wgpu::TextureFormat::RGBA8Unorm,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  const wgpu::TextureViewDescriptor textureViewDescriptor{
      .label = "imgui texture view",
      .format = wgpu::TextureFormat::RGBA8Unorm,
      .dimension = wgpu::TextureViewDimension::e2D,
      .mipLevelCount = WGPU_MIP_LEVEL_COUNT_UNDEFINED,
      .arrayLayerCount = WGPU_ARRAY_LAYER_COUNT_UNDEFINED,
  };
  auto texture = webgpu::g_device.CreateTexture(&textureDescriptor);
  auto textureView = texture.CreateView(&textureViewDescriptor);
  {
    const wgpu::TexelCopyTextureInfo dstView{
        .texture = texture,
    };
    const wgpu::TexelCopyBufferLayout dataLayout{
        .bytesPerRow = 4 * width,
        .rowsPerImage = height,
    };
    webgpu::g_queue.WriteTexture(&dstView, data, width * height * 4, &dataLayout, &size);
  }
  g_wgpuTextures.push_back(texture);
  return reinterpret_cast<ImTextureID>(textureView.MoveToCHandle());
}
} // namespace aurora::imgui

// C bindings
extern "C" {
ImTextureID aurora_imgui_add_texture(uint32_t width, uint32_t height, const void* rgba8) {
  return aurora::imgui::add_texture(width, height, static_cast<const uint8_t*>(rgba8));
}
ImFont* aurora_imgui_ui_font(float pixelSize) { return aurora::imgui::ui_font(pixelSize); }
}
