// Copyright 2021 Intel Corporation
// Copyright 2021 The Khronos Group
// SPDX-License-Identifier: Apache-2.0

#include "Frame.hpp"
#include "OSPRayDevice.hpp"
// std
#include <cstring>
#include <thread>

namespace anari {
namespace ospray {

Frame::Frame() : Object(nullptr, "<none>", CommitPriority::FRAME) {}

Frame::~Frame()
{
  ospRelease(m_future);
}

inline bool Frame::getProperty(
    const std::string &name, ANARIDataType type, void *ptr, uint32_t flags)
{
  if (name == "duration" && type == ANARI_FLOAT32 && m_future) {
    if (flags & ANARI_WAIT)
      ospWait(m_future);
    float d = ospGetTaskDuration(m_future);
    std::memcpy(ptr, &d, sizeof(d));
    return true;
  }

  if (name == "progress" && type == ANARI_FLOAT32 && m_future) {
    if (flags & ANARI_WAIT)
      ospWait(m_future);
    float d = ospGetProgress(m_future);
    std::memcpy(ptr, &d, sizeof(d));
    return true;
  }

  if (name == "variance" && type == ANARI_FLOAT32 && m_future) {
    if (flags & ANARI_WAIT)
      ospWait(m_future);
    float d = ospGetVariance(handleAs<OSPFrameBuffer>());
    std::memcpy(ptr, &d, sizeof(d));
    return true;
  }

  return Object::getProperty(name, type, ptr, flags);
}

void Frame::commit()
{
  if (m_reconstructOnCommit) {
    constructHandle();
    m_reconstructOnCommit = false;
  }

  Object::commit();
}

void Frame::setParam(const char *_id, ANARIDataType type, const void *mem)
{
  std::string id(_id);

  // XXX(th): These should report a warning to the user that non-prefixed names
  // aren't supported any more.
  if (id == "color") {
    id = "channel.color";
  } else if (id == "depth") {
    id = "channel.depth";
  } else if (id == "normal") {
    id = "channel.normal";
  } else if (id == "albedo") {
    id = "channel.albedo";
  }

  auto setAsInt = [&](int &v, const size_t offs = 0) {
    v = *((const int *)mem + offs);
    m_reconstructOnCommit = true;
  };

  auto setNewHandle = [](IntrusivePtr<Object> &existing, const void *mem) {
    existing = *(Object **)mem;
  };

  if (id == "size" && type == ANARI_UINT32_VEC2) {
    setAsInt(m_size_x);
    setAsInt(m_size_y, 1);
  } else if (id == "channel.color" && type == ANARI_DATA_TYPE)
    setAsInt(m_format);
  else if (id == "channel.depth" && type == ANARI_DATA_TYPE)
    m_depthBuffer = m_reconstructOnCommit = true;
  else if (id == "channel.albedo" && type == ANARI_DATA_TYPE)
    if (*(const ANARIDataType *)mem == ANARI_FLOAT32_VEC3)
      m_albedoBuffer = m_reconstructOnCommit = true;
    else
      throw std::runtime_error("Unsupported albedo type.");
  else if (id == "channel.normal" && type == ANARI_DATA_TYPE)
    if (*(const ANARIDataType *)mem == ANARI_FLOAT32_VEC3)
      m_normalBuffer = m_reconstructOnCommit = true;
    else
      throw std::runtime_error("Unsupported normal type.");
  else if (id == "renderer" && type == ANARI_RENDERER)
    setNewHandle(m_renderer, mem);
  else if (id == "camera" && type == ANARI_CAMERA)
    setNewHandle(m_camera, mem);
  else if (id == "world" && type == ANARI_WORLD)
    setNewHandle(m_world, mem);
  else if (id == "frameCompletionCallback"
      && type == ANARI_FRAME_COMPLETION_CALLBACK)
    m_continuation = *(ANARIFrameCompletionCallback *)mem;
  else if (id == "frameCompletionCallbackUserData"
      && type == ANARI_VOID_POINTER)
    m_continuationPtr = (void *)mem;
  else
    Object::setParam(_id, type, mem);
}

void Frame::render(ANARIDevice _d)
{
  OSPRayDevice *d = (OSPRayDevice *)_d;

  ospRelease(m_future);

  if (d->isModified())
    ospResetAccumulation(handleAs<OSPFrameBuffer>());
  m_future = ospRenderFrame(handleAs<OSPFrameBuffer>(),
      m_renderer->handleAs<OSPRenderer>(),
      m_camera->handleAs<OSPCamera>(),
      m_world->handleAs<OSPWorld>());

  if (m_continuation) {
    ospWait(m_future);
    auto *f = this;
    f->refInc();
    std::thread([=]() {
      m_continuation(m_continuationPtr, _d, (ANARIFrame)this);
      f->refDec();
    }).detach();
  }
}

OSPFuture Frame::future()
{
  return m_future;
}

const void *Frame::map(const std::string &_channel,
    uint32_t *width,
    uint32_t *height,
    ANARIDataType *pixelType)
{
  auto fbh = handleAs<OSPFrameBuffer>();
  *width = m_size_x;
  *height = m_size_y;

  std::string channel(_channel);

  // XXX(th): These should report a warning to the user that non-prefixed names
  // aren't supported any more.
  if (channel == "color") {
    channel = "channel.color";
  } else if (channel == "depth") {
    channel = "channel.depth";
  } else if (channel == "albedo") {
    channel = "channel.albedo";
  } else if (channel == "normal") {
    channel = "channel.normal";
  }

  if (channel == "channel.color") {
    *pixelType = m_format;
    return ospMapFrameBuffer(fbh, OSP_FB_COLOR);
  } else if (channel == "channel.depth") {
    *pixelType = ANARI_FLOAT32;
    return ospMapFrameBuffer(fbh, OSP_FB_DEPTH);
  } else if (channel == "channel.albedo") {
    // TODO: support other channel types here?
    *pixelType = ANARI_FLOAT32_VEC3;
    return ospMapFrameBuffer(fbh, OSP_FB_ALBEDO);
  } else if (channel == "channel.normal") {
    // TODO: support other channel types here?
    *pixelType = ANARI_FLOAT32_VEC3;
    return ospMapFrameBuffer(fbh, OSP_FB_NORMAL);
  } else
    return nullptr;
}

void Frame::unmap(const char *ptr)
{
  auto fbh = handleAs<OSPFrameBuffer>();
  ospUnmapFrameBuffer(ptr, fbh);
}

void Frame::constructHandle()
{
  ospRelease(m_object);

  auto channels = OSP_FB_COLOR | OSP_FB_ACCUM | OSP_FB_VARIANCE;
  if (m_depthBuffer)
    channels |= OSP_FB_DEPTH;
  if (m_normalBuffer)
    channels |= OSP_FB_NORMAL;
  if (m_albedoBuffer)
    channels |= OSP_FB_ALBEDO;

  m_object = ospNewFrameBuffer(
      m_size_x, m_size_y, enumCast<OSPFrameBufferFormat>(m_format), channels);
}

} // namespace ospray
} // namespace anari
