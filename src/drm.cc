/*
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */


#include "drm.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <glob.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>

DrmBuffer DrmBuffer::allocate(size_t size)
{
    std::string tried;

    glob_t found = {};
    std::vector<std::string> paths;
    if (glob("/dev/dri/card*", 0, nullptr, &found) == 0) {
        for (size_t i = 0; i < found.gl_pathc; i++) {
            paths.push_back(found.gl_pathv[i]);
        }
    }
    globfree(&found);

    for (const auto& path : paths) {
        const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            tried += " " + path + ": " + strerror(errno) + ";";
            continue;
        }

        // Dumb buffers are two dimensional: a wide 8 bit "image" with enough rows to hold `size` bytes.
        drm_mode_create_dumb create = {};
        create.width = 4096;
        create.height = static_cast<uint32_t>((size + create.width - 1) / create.width);
        create.bpp = 8;
        if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
            tried += " " + path + ": creating a buffer: " + strerror(errno) + ";";
            close(fd);
            continue;
        }

        const auto release = [&] {
            drm_mode_destroy_dumb destroy = { create.handle };
            ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
            close(fd);
        };

        drm_mode_map_dumb map = {};
        map.handle = create.handle;
        if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
            tried += " " + path + ": mapping: " + strerror(errno) + ";";
            release();
            continue;
        }
        void* memory = mmap(nullptr, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
        if (memory == MAP_FAILED) {
            tried += " " + path + ": mmap: " + strerror(errno) + ";";
            release();
            continue;
        }

        drm_prime_handle prime = {};
        prime.handle = create.handle;
        prime.flags = DRM_CLOEXEC | DRM_RDWR;
        if (ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0) {
            tried += " " + path + ": exporting: " + strerror(errno) + ";";
            munmap(memory, create.size);
            release();
            continue;
        }

        return DrmBuffer(fd, create.handle, static_cast<uint8_t*>(memory), create.size, prime.fd);
    }

    throw std::runtime_error("no DRM device (/dev/dri/card*) could provide an exportable buffer:"
        + (tried.empty() ? std::string(" none found") : tried));
}

DrmBuffer::DrmBuffer(DrmBuffer&& other) noexcept
    : drm_fd_(std::exchange(other.drm_fd_, -1))
    , handle_(other.handle_)
    , map_(std::exchange(other.map_, nullptr))
    , size_(std::exchange(other.size_, 0))
    , dmabuf_fd_(std::exchange(other.dmabuf_fd_, -1))
{
}

DrmBuffer::~DrmBuffer()
{
    if (map_) {
        munmap(map_, size_);
    }
    if (dmabuf_fd_ >= 0) {
        close(dmabuf_fd_);
    }
    if (drm_fd_ >= 0) {
        // (An exported DMA-BUF keeps the memory alive for as long as someone uses it.)
        drm_mode_destroy_dumb destroy = { handle_ };
        ioctl(drm_fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        close(drm_fd_);
    }
}

int DrmBuffer::duplicate_fd() const
{
    const int result = fcntl(dmabuf_fd_, F_DUPFD_CLOEXEC, 0);
    if (result < 0) {
        throw std::runtime_error(std::string("Unable to duplicate DMA-BUF descriptor: ") + strerror(errno));
    }
    return result;
}
