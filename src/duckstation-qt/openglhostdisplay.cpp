#include "openglhostdisplay.h"
#include "common/assert.h"
#include "common/log.h"
#include "imgui.h"
#include "qtdisplaywidget.h"
#include "qthostinterface.h"
#include <QtCore/QDebug>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QWindow>
#include <array>
#include <imgui_impl_opengl3.h>
#if !defined(WIN32) && !defined(APPLE)
#include <qpa/qplatformnativeinterface.h>
#endif
#include <tuple>
Log_SetChannel(OpenGLHostDisplay);

class OpenGLDisplayWidgetTexture : public HostDisplayTexture
{
public:
  OpenGLDisplayWidgetTexture(GLuint id, u32 width, u32 height) : m_id(id), m_width(width), m_height(height) {}
  ~OpenGLDisplayWidgetTexture() override { glDeleteTextures(1, &m_id); }

  void* GetHandle() const override { return reinterpret_cast<void*>(static_cast<uintptr_t>(m_id)); }
  u32 GetWidth() const override { return m_width; }
  u32 GetHeight() const override { return m_height; }

  GLuint GetGLID() const { return m_id; }

  static std::unique_ptr<OpenGLDisplayWidgetTexture> Create(u32 width, u32 height, const void* initial_data,
                                                            u32 initial_data_stride)
  {
    GLuint id;
    glGenTextures(1, &id);

    GLint old_texture_binding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture_binding);

    // TODO: Set pack width
    Assert(!initial_data || initial_data_stride == (width * sizeof(u32)));

    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, initial_data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindTexture(GL_TEXTURE_2D, id);
    return std::make_unique<OpenGLDisplayWidgetTexture>(id, width, height);
  }

private:
  GLuint m_id;
  u32 m_width;
  u32 m_height;
};

OpenGLHostDisplay::OpenGLHostDisplay(QtHostInterface* host_interface) : QtHostDisplay(host_interface) {}

OpenGLHostDisplay::~OpenGLHostDisplay() = default;

QtDisplayWidget* OpenGLHostDisplay::createWidget(QWidget* parent)
{
  QtDisplayWidget* widget = QtHostDisplay::createWidget(parent);

  QWindow* native_window = widget->windowHandle();
  Assert(native_window);
  native_window->setSurfaceType(QWindow::OpenGLSurface);

  return widget;
}

HostDisplay::RenderAPI OpenGLHostDisplay::GetRenderAPI() const
{
  return m_gl_context->IsGLES() ? HostDisplay::RenderAPI::OpenGLES : HostDisplay::RenderAPI::OpenGL;
}

void* OpenGLHostDisplay::GetRenderDevice() const
{
  return nullptr;
}

void* OpenGLHostDisplay::GetRenderContext() const
{
  return m_gl_context.get();
}

void OpenGLHostDisplay::WindowResized(s32 new_window_width, s32 new_window_height)
{
  QtHostDisplay::WindowResized(new_window_width, new_window_height);
  m_gl_context->ResizeSurface(static_cast<u32>(new_window_width), static_cast<u32>(new_window_height));
}

std::unique_ptr<HostDisplayTexture> OpenGLHostDisplay::CreateTexture(u32 width, u32 height, const void* initial_data,
                                                                     u32 initial_data_stride, bool dynamic)
{
  return OpenGLDisplayWidgetTexture::Create(width, height, initial_data, initial_data_stride);
}

void OpenGLHostDisplay::UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height,
                                      const void* texture_data, u32 texture_data_stride)
{
  OpenGLDisplayWidgetTexture* tex = static_cast<OpenGLDisplayWidgetTexture*>(texture);
  Assert((texture_data_stride % sizeof(u32)) == 0);

  GLint old_texture_binding = 0, old_alignment = 0, old_row_length = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture_binding);
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &old_alignment);
  glGetIntegerv(GL_UNPACK_ROW_LENGTH, &old_row_length);

  glBindTexture(GL_TEXTURE_2D, tex->GetGLID());
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, texture_data_stride / sizeof(u32));

  glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, texture_data);

  glPixelStorei(GL_UNPACK_ALIGNMENT, old_alignment);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, old_row_length);
  glBindTexture(GL_TEXTURE_2D, old_texture_binding);
}

bool OpenGLHostDisplay::DownloadTexture(const void* texture_handle, u32 x, u32 y, u32 width, u32 height, void* out_data,
                                        u32 out_data_stride)
{
  GLint old_alignment = 0, old_row_length = 0;
  glGetIntegerv(GL_PACK_ALIGNMENT, &old_alignment);
  glGetIntegerv(GL_PACK_ROW_LENGTH, &old_row_length);
  glPixelStorei(GL_PACK_ALIGNMENT, sizeof(u32));
  glPixelStorei(GL_PACK_ROW_LENGTH, out_data_stride / sizeof(u32));

  const GLuint texture = static_cast<GLuint>(reinterpret_cast<uintptr_t>(texture_handle));
  GL::Texture::GetTextureSubImage(texture, 0, x, y, 0, width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                  height * out_data_stride, out_data);

  glPixelStorei(GL_PACK_ALIGNMENT, old_alignment);
  glPixelStorei(GL_PACK_ROW_LENGTH, old_row_length);
  return true;
}

void OpenGLHostDisplay::SetVSync(bool enabled)
{
  // Window framebuffer has to be bound to call SetSwapInterval.
  GLint current_fbo = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  m_gl_context->SetSwapInterval(enabled ? 1 : 0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, current_fbo);
}

const char* OpenGLHostDisplay::GetGLSLVersionString() const
{
  if (m_gl_context->IsGLES())
  {
    if (GLAD_GL_ES_VERSION_3_0)
      return "#version 300 es";
    else
      return "#version 100";
  }
  else
  {
    if (GLAD_GL_VERSION_3_3)
      return "#version 330";
    else
      return "#version 130";
  }
}

std::string OpenGLHostDisplay::GetGLSLVersionHeader() const
{
  std::string header = GetGLSLVersionString();
  header += "\n\n";
  if (m_gl_context->IsGLES())
  {
    header += "precision highp float;\n";
    header += "precision highp int;\n\n";
  }

  return header;
}

static void APIENTRY GLDebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                     const GLchar* message, const void* userParam)
{
  switch (severity)
  {
    case GL_DEBUG_SEVERITY_HIGH_KHR:
      Log_ErrorPrintf(message);
      break;
    case GL_DEBUG_SEVERITY_MEDIUM_KHR:
      Log_WarningPrint(message);
      break;
    case GL_DEBUG_SEVERITY_LOW_KHR:
      Log_InfoPrintf(message);
      break;
    case GL_DEBUG_SEVERITY_NOTIFICATION:
      // Log_DebugPrint(message);
      break;
  }
}

bool OpenGLHostDisplay::hasDeviceContext() const
{
  return static_cast<bool>(m_gl_context);
}

WindowInfo OpenGLHostDisplay::getWindowInfo() const
{
  WindowInfo wi;

  // Windows and Apple are easy here since there's no display connection.
#if defined(WIN32)
  wi.type = WindowInfo::Type::Win32;
  wi.window_handle = reinterpret_cast<void*>(m_widget->winId());
#elif defined(__APPLE__)
  wi.type = WindowInfo::Type::MacOS;
  wi.window_handle = reinterpret_cast<void*>(m_widget->winId());
#else
  QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
  const QString platform_name = QGuiApplication::platformName();
  if (platform_name == QStringLiteral("xcb"))
  {
    wi.type = WindowInfo::Type::X11;
    wi.display_connection = pni->nativeResourceForWindow("display", m_widget->windowHandle());
    wi.window_handle = reinterpret_cast<void*>(m_widget->winId());
  }
  else if (platform_name == QStringLiteral("wayland"))
  {
    wi.type = WindowInfo::Type::Wayland;
    wi.display_connection = pni->nativeResourceForWindow("display", m_widget->windowHandle());
    wi.window_handle = pni->nativeResourceForWindow("surface", m_widget->windowHandle());
  }
  else
  {
    qCritical() << "Unknown PNI platform " << platform_name;
    return wi;
  }
#endif

  wi.surface_width = m_widget->width();
  wi.surface_height = m_widget->height();
  wi.surface_format = WindowInfo::SurfaceFormat::RGB8;

  return wi;
}

bool OpenGLHostDisplay::createDeviceContext(bool debug_device)
{
  m_gl_context = GL::Context::Create(getWindowInfo());
  if (!m_gl_context)
  {
    Log_ErrorPrintf("Failed to create any GL context");
    return false;
  }

  return true;
}

bool OpenGLHostDisplay::initializeDeviceContext(bool debug_device)
{
  if (debug_device && GLAD_GL_KHR_debug)
  {
    glad_glDebugMessageCallbackKHR(GLDebugCallback, nullptr);
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
  }

  if (!QtHostDisplay::initializeDeviceContext(debug_device))
  {
    m_gl_context->DoneCurrent();
    return false;
  }

  return true;
}

bool OpenGLHostDisplay::activateDeviceContext()
{
  if (!m_gl_context->MakeCurrent())
  {
    Log_ErrorPrintf("Failed to make GL context current");
    return false;
  }

  return true;
}

void OpenGLHostDisplay::deactivateDeviceContext()
{
  m_gl_context->DoneCurrent();
}

void OpenGLHostDisplay::destroyDeviceContext()
{
  QtHostDisplay::destroyDeviceContext();
  m_gl_context->DoneCurrent();
  m_gl_context.reset();
}

bool OpenGLHostDisplay::createSurface()
{
  m_window_width = m_widget->scaledWindowWidth();
  m_window_height = m_widget->scaledWindowHeight();
  emit m_widget->windowResizedEvent(m_window_width, m_window_height);

  if (m_gl_context)
    m_gl_context->ChangeSurface(getWindowInfo());

  return true;
}

void OpenGLHostDisplay::destroySurface() {}

bool OpenGLHostDisplay::createImGuiContext()
{
  if (!QtHostDisplay::createImGuiContext())
    return false;

  if (!ImGui_ImplOpenGL3_Init(GetGLSLVersionString()))
    return false;

  ImGui_ImplOpenGL3_NewFrame();
  ImGui::NewFrame();
  return true;
}

void OpenGLHostDisplay::destroyImGuiContext()
{
  ImGui_ImplOpenGL3_Shutdown();

  QtHostDisplay::destroyImGuiContext();
}

bool OpenGLHostDisplay::createDeviceResources()
{
  static constexpr char fullscreen_quad_vertex_shader[] = R"(
uniform vec4 u_src_rect;
out vec2 v_tex0;

void main()
{
  vec2 pos = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  v_tex0 = u_src_rect.xy + pos * u_src_rect.zw;
  gl_Position = vec4(pos * vec2(2.0f, -2.0f) + vec2(-1.0f, 1.0f), 0.0f, 1.0f);
}
)";

  static constexpr char display_fragment_shader[] = R"(
uniform sampler2D samp0;

in vec2 v_tex0;
out vec4 o_col0;

void main()
{
  o_col0 = vec4(texture(samp0, v_tex0).rgb, 1.0);
}
)";

  if (!m_display_program.Compile(GetGLSLVersionHeader() + fullscreen_quad_vertex_shader, {},
                                 GetGLSLVersionHeader() + display_fragment_shader))
  {
    Log_ErrorPrintf("Failed to compile display shaders");
    return false;
  }

  if (!m_gl_context->IsGLES())
    m_display_program.BindFragData(0, "o_col0");

  if (!m_display_program.Link())
  {
    Log_ErrorPrintf("Failed to link display program");
    return false;
  }

  m_display_program.Bind();
  m_display_program.RegisterUniform("u_src_rect");
  m_display_program.RegisterUniform("samp0");
  m_display_program.Uniform1i(1, 0);

  glGenVertexArrays(1, &m_display_vao);

  // samplers
  glGenSamplers(1, &m_display_nearest_sampler);
  glSamplerParameteri(m_display_nearest_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glSamplerParameteri(m_display_nearest_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glGenSamplers(1, &m_display_linear_sampler);
  glSamplerParameteri(m_display_linear_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glSamplerParameteri(m_display_linear_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  return true;
}

void OpenGLHostDisplay::destroyDeviceResources()
{
  QtHostDisplay::destroyDeviceResources();

  if (m_display_vao != 0)
    glDeleteVertexArrays(1, &m_display_vao);
  if (m_display_linear_sampler != 0)
    glDeleteSamplers(1, &m_display_linear_sampler);
  if (m_display_nearest_sampler != 0)
    glDeleteSamplers(1, &m_display_nearest_sampler);

  m_display_program.Destroy();
}

void OpenGLHostDisplay::Render()
{
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  renderDisplay();

  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

  m_gl_context->SwapBuffers();

  ImGui::NewFrame();
  ImGui_ImplOpenGL3_NewFrame();

  GL::Program::ResetLastProgram();
}

void OpenGLHostDisplay::renderDisplay()
{
  if (!m_display_texture_handle)
    return;

  const auto [vp_left, vp_top, vp_width, vp_height] =
    CalculateDrawRect(m_window_width, m_window_height, m_display_top_margin);

  glViewport(vp_left, m_window_height - (m_display_top_margin + vp_top) - vp_height, vp_width, vp_height);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDepthMask(GL_FALSE);
  m_display_program.Bind();
  m_display_program.Uniform4f(
    0, static_cast<float>(m_display_texture_view_x) / static_cast<float>(m_display_texture_width),
    static_cast<float>(m_display_texture_view_y) / static_cast<float>(m_display_texture_height),
    (static_cast<float>(m_display_texture_view_width) - 0.5f) / static_cast<float>(m_display_texture_width),
    (static_cast<float>(m_display_texture_view_height) + 0.5f) / static_cast<float>(m_display_texture_height));
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(reinterpret_cast<uintptr_t>(m_display_texture_handle)));
  glBindSampler(0, m_display_linear_filtering ? m_display_linear_sampler : m_display_nearest_sampler);
  glBindVertexArray(m_display_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindSampler(0, 0);
}
