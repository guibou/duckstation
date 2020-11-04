#include "drm_display.h"
#include "common/log.h"
#include "common/string.h"
#include "common/assert.h"
#include "file_system.h"
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
Log_SetChannel(DRMDisplay);

DRMDisplay::DRMDisplay(int card /*= 1*/) : m_card_id(card) {}

DRMDisplay::~DRMDisplay()
{
  while (m_num_buffers > 0)
  {
    Buffer& buffer = m_buffers[--m_num_buffers];
    drmModeRmFB(m_card_fd, buffer.fb_id);
  }

  if (m_fb_surface)
    gbm_surface_destroy(m_fb_surface);

  if (m_gbm_device)
    gbm_device_destroy(m_gbm_device);

  if (m_connector)
    drmModeFreeConnector(m_connector);

  if (m_card_fd >= 0)
    close(m_card_fd);
}

// https://gist.github.com/Miouyouyou/89e9fe56a2c59bce7d4a18a858f389ef

static uint32_t find_crtc_for_encoder(const drmModeRes* resources, const drmModeEncoder* encoder)
{
  int i;

  for (i = 0; i < resources->count_crtcs; i++)
  {
    /* possible_crtcs is a bitmask as described here:
     * https://dvdhrm.wordpress.com/2012/09/13/linux-drm-mode-setting-api
     */
    const uint32_t crtc_mask = 1 << i;
    const uint32_t crtc_id = resources->crtcs[i];
    if (encoder->possible_crtcs & crtc_mask)
    {
      return crtc_id;
    }
  }

  /* no match found */
  return -1;
}

static uint32_t find_crtc_for_connector(int card_fd, const drmModeRes* resources, const drmModeConnector* connector)
{
  int i;

  for (i = 0; i < connector->count_encoders; i++)
  {
    const uint32_t encoder_id = connector->encoders[i];
    drmModeEncoder* encoder = drmModeGetEncoder(card_fd, encoder_id);

    if (encoder)
    {
      const uint32_t crtc_id = find_crtc_for_encoder(resources, encoder);

      drmModeFreeEncoder(encoder);
      if (crtc_id != 0)
      {
        return crtc_id;
      }
    }
  }

  /* no match found */
  return -1;
}

bool DRMDisplay::Initialize()
{
  m_card_fd = open(TinyString::FromFormat("/dev/dri/card%d", m_card_id), O_RDWR);
  if (m_card_fd < 0)
  {
    Log_ErrorPrintf("open(/dev/dri/card%d) failed: %d (%s)", m_card_id, errno, strerror(errno));
    return false;
  }

  drmModeRes* resources = drmModeGetResources(m_card_fd);
  if (!resources)
  {
    Log_ErrorPrintf("drmModeGetResources() failed: %d (%s)", errno, strerror(errno));
    return false;
  }

  Assert(!m_connector);

  for (int i = 0; i < resources->count_connectors; i++)
  {
    drmModeConnector* next_connector = drmModeGetConnector(m_card_fd, resources->connectors[i]);
    if (next_connector->connection == DRM_MODE_CONNECTED)
    {
      m_connector = next_connector;
      break;
    }

    drmModeFreeConnector(next_connector);
  }

  if (!m_connector)
  {
    Log_ErrorPrintf("No connector found");
    drmModeFreeResources(resources);
    return false;
  }

  for (int i = 0; i < m_connector->count_modes; i++)
  {
    drmModeModeInfo* next_mode = &m_connector->modes[i];
    if (next_mode->type & DRM_MODE_TYPE_PREFERRED)
    {
      m_mode = next_mode;
      break;
    }

    if (!m_mode || (next_mode->hdisplay * next_mode->vdisplay) > (m_mode->hdisplay * m_mode->vdisplay))
    {
      m_mode = next_mode;
    }
  }

  if (!m_mode)
  {
    Log_ErrorPrintf("No mode found");
    drmModeFreeResources(resources);
    return false;
  }

  drmModeEncoder* encoder = nullptr;
  for (int i = 0; i < resources->count_encoders; i++)
  {
    drmModeEncoder* next_encoder = drmModeGetEncoder(m_card_fd, resources->encoders[i]);
    if (next_encoder->encoder_id == m_connector->encoder_id)
    {
      encoder = next_encoder;
      m_crtc_id = encoder->crtc_id;
      break;
    }

    drmModeFreeEncoder(next_encoder);
  }

  if (encoder)
  {
    drmModeFreeEncoder(encoder);
  }
  else
  {
    m_crtc_id = find_crtc_for_connector(m_card_fd, resources, m_connector);
    if (m_crtc_id == 0)
    {
      Log_ErrorPrintf("No CRTC found");
      drmModeFreeResources(resources);
      return false;
    }
  }

  drmModeFreeResources(resources);

  m_gbm_device = gbm_create_device(m_card_fd);
  if (!m_gbm_device)
  {
    Log_ErrorPrintf("gbm_create_device() failed: %d", errno);
    return false;
  }

  return true;
}

struct gbm_surface* DRMDisplay::CreateFramebufferSurface(u32 fourcc, u32 flags)
{
  Assert(!m_fb_surface);

  m_fb_surface = gbm_surface_create(m_gbm_device, GetWidth(), GetHeight(), fourcc, flags | GBM_BO_USE_SCANOUT);
  if (!m_fb_surface)
  {
    Log_ErrorPrintf("gbm_surface_create() failed: %d", errno);
    return nullptr;
  }

  return m_fb_surface;
}

DRMDisplay::Buffer* DRMDisplay::LockFrontBuffer()
{
  struct gbm_bo* bo = gbm_surface_lock_front_buffer(m_fb_surface);

  Buffer* buffer = nullptr;
  for (u32 i = 0; i < m_num_buffers; i++)
  {
    if (m_buffers[i].bo == bo)
    {
      buffer = &m_buffers[i];
      break;
    }
  }

  if (!buffer)
  {
    // haven't tracked this buffer yet
    Assert(m_num_buffers < MAX_BUFFERS);
    buffer = &m_buffers[m_num_buffers];
    buffer->bo = bo;
    buffer->width = gbm_bo_get_width(bo);
    buffer->height = gbm_bo_get_height(bo);
    buffer->stride = gbm_bo_get_stride(bo);
    buffer->format = gbm_bo_get_format(bo);

    uint32_t bo_handles[4] = {gbm_bo_get_handle(bo).u32, 0, 0, 0};
    uint32_t pitches[4] = {buffer->stride, 0, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};

    int res = drmModeAddFB2(m_card_fd, buffer->width, buffer->height, buffer->format, bo_handles, pitches, offsets,
                            &buffer->fb_id, 0);
    if (res != 0)
    {
      Log_ErrorPrintf("drmModeAddFB2() failed: %d", res);
      return nullptr;
    }

    m_num_buffers++;
  }

  return buffer;
}

void DRMDisplay::ReleaseBuffer(Buffer* buffer)
{
  gbm_surface_release_buffer(m_fb_surface, buffer->bo);
}

void DRMDisplay::PresentSurface(Buffer* buffer, bool wait_for_vsync)
{
  if (!wait_for_vsync)
  {
    u32 connector_id = m_connector->connector_id;
    int res = drmModeSetCrtc(m_card_fd, m_crtc_id, buffer->fb_id, 0, 0, &connector_id, 1, m_mode);
    if (res != 0)
      Log_ErrorPrintf("drmModeSetCrtc() failed: %d", res);

    return;
  }

  bool waiting_for_flip = true;
  drmEventContext event_ctx = {};
  event_ctx.version = DRM_EVENT_CONTEXT_VERSION;
  event_ctx.page_flip_handler = [](int fd, unsigned int frame, unsigned int sec, unsigned int usec, void* data) {
    *reinterpret_cast<bool*>(data) = false;
  };

  int res = drmModePageFlip(m_card_fd, m_crtc_id, buffer->fb_id, DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip);
  if (res != 0)
  {
    Log_ErrorPrintf("drmModePageFlip() failed: %d", res);
    return;
  }

  while (waiting_for_flip)
  {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(m_card_fd, &fds);
    int res = select(m_card_fd + 1, &fds, nullptr, nullptr, nullptr);
    if (res < 0)
    {
      Log_ErrorPrintf("select() failed: %d", errno);
      break;
    }
    else if (res == 0)
    {
      continue;
    }

    drmHandleEvent(m_card_fd, &event_ctx);
  }
}