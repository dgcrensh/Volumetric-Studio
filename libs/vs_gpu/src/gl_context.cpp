// The platform layer of gl.hpp: WGL on Windows, nothing elsewhere yet.

#include "gl.hpp"

#include <fmt/format.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h's MemoryBarrier() macro would rename the GL entry point.
#undef MemoryBarrier
#endif

namespace vs::gpu::gl {

#if defined(_WIN32)

namespace {

constexpr wchar_t kClassName[] = L"vs_gpu_hidden";

class WglContext final : public Context {
 public:
  ~WglContext() override {
    if (rc_ != nullptr) {
      wglMakeCurrent(nullptr, nullptr);
      wglDeleteContext(rc_);
    }
    if (dc_ != nullptr) ReleaseDC(window_, dc_);
    if (window_ != nullptr) DestroyWindow(window_);
    if (opengl32_ != nullptr) FreeLibrary(opengl32_);
  }

  bool open(std::string& why) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    RegisterClassW(&wc);  // fails harmlessly when already registered
    window_ = CreateWindowExW(0, kClassName, L"vs_gpu", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr,
                              nullptr, instance, nullptr);
    if (window_ == nullptr) return fail(why, "can't create a hidden window");
    dc_ = GetDC(window_);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    const int format = ChoosePixelFormat(dc_, &pfd);
    if (format == 0 || !SetPixelFormat(dc_, format, &pfd)) return fail(why, "no OpenGL pixel format");
    rc_ = wglCreateContext(dc_);
    if (rc_ == nullptr || !wglMakeCurrent(dc_, rc_)) return fail(why, "can't create an OpenGL context");
    opengl32_ = LoadLibraryW(L"opengl32.dll");
    if (!load(why)) return false;
    return true;
  }

  [[nodiscard]] const Functions& gl() const override { return f_; }

 private:
  bool fail(std::string& why, const char* what) {
    why = what;
    return false;
  }

  void* proc(const char* name) const {
    void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
    const auto bad = reinterpret_cast<std::intptr_t>(p);
    if (bad == 0 || bad == 1 || bad == 2 || bad == 3 || bad == -1) {
      p = opengl32_ != nullptr ? reinterpret_cast<void*>(GetProcAddress(opengl32_, name)) : nullptr;
    }
    return p;
  }

  template <typename T>
  bool get(T& slot, const char* name, std::string& why) {
    slot = reinterpret_cast<T>(proc(name));
    if (slot == nullptr) {
      why = fmt::format("the driver has no {}", name);
      return false;
    }
    return true;
  }

  bool load(std::string& why) {
    return get(f_.GetString, "glGetString", why) && get(f_.GetIntegerv, "glGetIntegerv", why) &&
           get(f_.GetError, "glGetError", why) && get(f_.Finish, "glFinish", why) &&
           get(f_.CreateShader, "glCreateShader", why) && get(f_.ShaderSource, "glShaderSource", why) &&
           get(f_.CompileShader, "glCompileShader", why) && get(f_.GetShaderiv, "glGetShaderiv", why) &&
           get(f_.GetShaderInfoLog, "glGetShaderInfoLog", why) &&
           get(f_.DeleteShader, "glDeleteShader", why) && get(f_.CreateProgram, "glCreateProgram", why) &&
           get(f_.AttachShader, "glAttachShader", why) && get(f_.LinkProgram, "glLinkProgram", why) &&
           get(f_.GetProgramiv, "glGetProgramiv", why) &&
           get(f_.GetProgramInfoLog, "glGetProgramInfoLog", why) &&
           get(f_.DeleteProgram, "glDeleteProgram", why) && get(f_.UseProgram, "glUseProgram", why) &&
           get(f_.GetUniformLocation, "glGetUniformLocation", why) &&
           get(f_.Uniform1ui, "glUniform1ui", why) && get(f_.GenBuffers, "glGenBuffers", why) &&
           get(f_.DeleteBuffers, "glDeleteBuffers", why) && get(f_.BindBuffer, "glBindBuffer", why) &&
           get(f_.BufferData, "glBufferData", why) && get(f_.BindBufferBase, "glBindBufferBase", why) &&
           get(f_.GetBufferSubData, "glGetBufferSubData", why) &&
           get(f_.DispatchCompute, "glDispatchCompute", why) && get(f_.MemoryBarrier, "glMemoryBarrier", why);
  }

  HWND window_ = nullptr;
  HDC dc_ = nullptr;
  HGLRC rc_ = nullptr;
  HMODULE opengl32_ = nullptr;
  Functions f_;
};

}  // namespace

std::unique_ptr<Context> make_context(std::string& why) {
  auto c = std::make_unique<WglContext>();
  if (!c->open(why)) return nullptr;
  GLint major = 0, minor = 0;
  c->gl().GetIntegerv(kMajorVersion, &major);
  c->gl().GetIntegerv(kMinorVersion, &minor);
  if (major < 4 || (major == 4 && minor < 3)) {
    why = fmt::format("OpenGL {}.{} has no compute shaders (4.3 needed)", major, minor);
    return nullptr;
  }
  return c;
}

#else

std::unique_ptr<Context> make_context(std::string& why) {
  why = "GPU sampling is only implemented on Windows so far";
  return nullptr;
}

#endif

}  // namespace vs::gpu::gl
