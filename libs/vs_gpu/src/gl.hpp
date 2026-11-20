#pragma once

// Private: a headless OpenGL 4.3+ context for compute shaders, and the few
// GL entry points vs_gpu uses, loaded at run time. No GL header is needed
// (the types and constants are declared here), so the library builds
// anywhere; the context itself exists only where a platform layer is
// written (Windows, WGL). Elsewhere make_context() says why there is none.
//
// A context is current on the thread that made it and must only be used
// there: vs_gpu keeps one worker thread per context.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace vs::gpu::gl {

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLbitfield = unsigned int;
using GLchar = char;
using GLboolean = unsigned char;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;
using GLuint64 = std::uint64_t;

inline constexpr GLenum kComputeShader = 0x91B9;
inline constexpr GLenum kCompileStatus = 0x8B81;
inline constexpr GLenum kLinkStatus = 0x8B82;
inline constexpr GLenum kInfoLogLength = 0x8B84;
inline constexpr GLenum kShaderStorageBuffer = 0x90D2;
inline constexpr GLenum kStaticDraw = 0x88E4;
inline constexpr GLenum kDynamicRead = 0x88E9;
inline constexpr GLenum kShaderStorageBarrierBit = 0x00002000;
inline constexpr GLenum kBufferUpdateBarrierBit = 0x00000200;
inline constexpr GLenum kVendor = 0x1F00;
inline constexpr GLenum kRenderer = 0x1F01;
inline constexpr GLenum kVersion = 0x1F02;
inline constexpr GLenum kMajorVersion = 0x821B;
inline constexpr GLenum kMinorVersion = 0x821C;
inline constexpr GLenum kNoError = 0;

#if defined(_WIN32)
#define VS_GL_API __stdcall
#else
#define VS_GL_API
#endif

struct Functions {
  const unsigned char*(VS_GL_API* GetString)(GLenum) = nullptr;
  void(VS_GL_API* GetIntegerv)(GLenum, GLint*) = nullptr;
  GLenum(VS_GL_API* GetError)() = nullptr;
  void(VS_GL_API* Finish)() = nullptr;
  GLuint(VS_GL_API* CreateShader)(GLenum) = nullptr;
  void(VS_GL_API* ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
  void(VS_GL_API* CompileShader)(GLuint) = nullptr;
  void(VS_GL_API* GetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
  void(VS_GL_API* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
  void(VS_GL_API* DeleteShader)(GLuint) = nullptr;
  GLuint(VS_GL_API* CreateProgram)() = nullptr;
  void(VS_GL_API* AttachShader)(GLuint, GLuint) = nullptr;
  void(VS_GL_API* LinkProgram)(GLuint) = nullptr;
  void(VS_GL_API* GetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
  void(VS_GL_API* GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
  void(VS_GL_API* DeleteProgram)(GLuint) = nullptr;
  void(VS_GL_API* UseProgram)(GLuint) = nullptr;
  GLint(VS_GL_API* GetUniformLocation)(GLuint, const GLchar*) = nullptr;
  void(VS_GL_API* Uniform1ui)(GLint, GLuint) = nullptr;
  void(VS_GL_API* GenBuffers)(GLsizei, GLuint*) = nullptr;
  void(VS_GL_API* DeleteBuffers)(GLsizei, const GLuint*) = nullptr;
  void(VS_GL_API* BindBuffer)(GLenum, GLuint) = nullptr;
  void(VS_GL_API* BufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
  void(VS_GL_API* BindBufferBase)(GLenum, GLuint, GLuint) = nullptr;
  void(VS_GL_API* GetBufferSubData)(GLenum, GLintptr, GLsizeiptr, void*) = nullptr;
  void(VS_GL_API* DispatchCompute)(GLuint, GLuint, GLuint) = nullptr;
  void(VS_GL_API* MemoryBarrier)(GLbitfield) = nullptr;
};

class Context {
 public:
  virtual ~Context() = default;
  [[nodiscard]] virtual const Functions& gl() const = 0;
};

// A context current on the calling thread, or null with `why` set.
std::unique_ptr<Context> make_context(std::string& why);

}  // namespace vs::gpu::gl
