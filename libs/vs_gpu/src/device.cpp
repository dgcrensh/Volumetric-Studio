#include "device.hpp"

#include <algorithm>
#include <memory>

#include <fmt/format.h>

namespace vs::gpu {

Device::Device() {
  bool ready = false;
  thread_ = std::thread([this, &ready] {
    std::unique_ptr<gl::Context> context = gl::make_context(why_);
    {
      std::lock_guard lock(mutex_);
      if (context) {
        const auto& f = context->gl();
        const auto str = [&](gl::GLenum e) {
          const unsigned char* s = f.GetString(e);
          return s != nullptr ? std::string(reinterpret_cast<const char*>(s)) : std::string();
        };
        description_ = fmt::format("{}, {}", str(gl::kRenderer), str(gl::kVersion));
        ok_ = true;
        context_ = context.get();
      }
      ready = true;
    }
    wake_.notify_all();
    if (context) loop();
    {
      std::lock_guard lock(mutex_);
      for (auto& [source, id] : programs_) context->gl().DeleteProgram(id);
      programs_.clear();
      context_ = nullptr;
    }
  });
  std::unique_lock lock(mutex_);
  wake_.wait(lock, [&] { return ready; });
}

Device::~Device() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void Device::loop() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
      if (jobs_.empty()) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    job();
  }
}

void Device::run(const std::function<void(const gl::Functions&)>& job) {
  if (!ok_) return;
  std::mutex done_mutex;
  std::condition_variable done_cv;
  bool done = false;
  {
    std::lock_guard lock(mutex_);
    jobs_.push_back([&] {
      job(context_->gl());
      {
        std::lock_guard l(done_mutex);
        done = true;
      }
      done_cv.notify_all();
    });
  }
  wake_.notify_all();
  std::unique_lock l(done_mutex);
  done_cv.wait(l, [&] { return done; });
}

gl::GLuint Device::program(const gl::Functions& f, const std::string& source, std::string& log) {
  if (const auto it = programs_.find(source); it != programs_.end()) return it->second;
  const gl::GLuint shader = f.CreateShader(gl::kComputeShader);
  const char* text = source.c_str();
  f.ShaderSource(shader, 1, &text, nullptr);
  f.CompileShader(shader);
  gl::GLint status = 0;
  f.GetShaderiv(shader, gl::kCompileStatus, &status);
  const auto info = [&](bool is_shader, gl::GLuint id) {
    gl::GLint length = 0;
    if (is_shader) {
      f.GetShaderiv(id, gl::kInfoLogLength, &length);
    } else {
      f.GetProgramiv(id, gl::kInfoLogLength, &length);
    }
    std::string s(static_cast<std::size_t>(std::max(length, 1)), '\0');
    if (is_shader) {
      f.GetShaderInfoLog(id, length, nullptr, s.data());
    } else {
      f.GetProgramInfoLog(id, length, nullptr, s.data());
    }
    return s;
  };
  if (status == 0) {
    log = info(true, shader);
    f.DeleteShader(shader);
    return 0;
  }
  const gl::GLuint program = f.CreateProgram();
  f.AttachShader(program, shader);
  f.LinkProgram(program);
  f.DeleteShader(shader);
  f.GetProgramiv(program, gl::kLinkStatus, &status);
  if (status == 0) {
    log = info(false, program);
    f.DeleteProgram(program);
    return 0;
  }
  programs_.emplace(source, program);
  return program;
}

Buffer::Buffer(const gl::Functions& f, std::size_t bytes, const void* data) : f_(f) {
  f_.GenBuffers(1, &id_);
  f_.BindBuffer(gl::kShaderStorageBuffer, id_);
  // Zero-sized buffers aren't allowed to bind; keep at least one word.
  f_.BufferData(gl::kShaderStorageBuffer, static_cast<gl::GLsizeiptr>(std::max<std::size_t>(bytes, 16)),
                bytes > 0 ? data : nullptr, gl::kDynamicRead);
}

Buffer::~Buffer() { f_.DeleteBuffers(1, &id_); }

void Buffer::bind(gl::GLuint binding) const { f_.BindBufferBase(gl::kShaderStorageBuffer, binding, id_); }

void Buffer::read(void* out, std::size_t bytes, std::size_t offset) const {
  f_.BindBuffer(gl::kShaderStorageBuffer, id_);
  f_.GetBufferSubData(gl::kShaderStorageBuffer, static_cast<gl::GLintptr>(offset),
                      static_cast<gl::GLsizeiptr>(bytes), out);
}

}  // namespace vs::gpu
