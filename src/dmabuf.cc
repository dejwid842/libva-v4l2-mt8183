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

#include "dmabuf.h"

#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

extern "C" {
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
}

namespace {

int alloc_from_heap(const std::string& path, size_t size)
{
    const int heap = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (heap < 0) {
        return -1;
    }

    dma_heap_allocation_data data = {};
    data.len = size;
    data.fd_flags = O_RDWR | O_CLOEXEC;
    const int ret = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &data);
    close(heap);

    return (ret < 0) ? -1 : static_cast<int>(data.fd);
}

int alloc_from_udmabuf(size_t size)
{
    const int device = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (device < 0) {
        return -1;
    }

    int result = -1;
    const int memfd = memfd_create("libva-v4l2", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (memfd >= 0) {
        if (ftruncate(memfd, size) == 0 && fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) == 0) {
            udmabuf_create create = {};
            create.memfd = memfd;
            create.flags = UDMABUF_FLAGS_CLOEXEC;
            create.offset = 0;
            create.size = size;
            result = ioctl(device, UDMABUF_CREATE, &create);
        }
        close(memfd);
    }
    close(device);

    return result;
}

void sync_cpu_access(int fd, uint64_t flags)
{
    dma_buf_sync sync = { .flags = flags };
    while (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0 && errno == EINTR) { }
    // Not all exporters implement this; there's nothing sensible to do about a failure anyway.
}

} // namespace

DmaBuffer DmaBuffer::allocate(size_t size, bool dmabuf_only)
{
    const size_t page = sysconf(_SC_PAGESIZE);
    size = (size + page - 1) & ~(page - 1);

    std::vector<std::string> heaps;
    if (const char* env = getenv("LIBVA_V4L2_DMA_HEAP"); env && *env) {
        heaps.push_back((env[0] == '/') ? std::string(env) : std::string("/dev/dma_heap/") + env);
    }
    heaps.push_back("/dev/dma_heap/system");

    int fd = -1;
    for (const auto& heap : heaps) {
        if ((fd = alloc_from_heap(heap, size)) >= 0) {
            break;
        }
    }
    if (fd < 0) {
        fd = alloc_from_udmabuf(size);
    }

    bool exportable = true;
    if (fd < 0 && dmabuf_only) {
        throw std::runtime_error("No DMA-BUF allocator (dma-heap or udmabuf) is available");
    }
    if (fd < 0) {
        // No DMA-BUF allocator: still good enough for software access to the converted pictures.
        exportable = false;
        fd = memfd_create("libva-v4l2", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, size) != 0) {
            const int error = errno;
            if (fd >= 0) {
                close(fd);
            }
            throw std::system_error(error, std::generic_category(), "Unable to allocate memory for format conversion");
        }
    }

    void* map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        const int error = errno;
        close(fd);
        throw std::system_error(error, std::generic_category(), "mmap of DMA-BUF failed");
    }

    return DmaBuffer(fd, size, static_cast<uint8_t*>(map), exportable);
}

DmaBuffer DmaBuffer::import(int fd, size_t size)
{
    if (size == 0) {
        const off_t end = lseek(fd, 0, SEEK_END);
        if (end <= 0) {
            throw std::runtime_error("Unable to find out the size of the DMA-BUF");
        }
        size = end;
    }

    const int duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) {
        throw std::system_error(errno, std::generic_category(), "Unable to duplicate DMA-BUF descriptor");
    }

    void* map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, duplicate, 0);
    if (map == MAP_FAILED) {
        const int error = errno;
        close(duplicate);
        throw std::system_error(error, std::generic_category(), "mmap of the application's DMA-BUF failed");
    }

    return DmaBuffer(duplicate, size, static_cast<uint8_t*>(map), true);
}

DmaBuffer::DmaBuffer(DmaBuffer&& other) noexcept
    : fd_(std::exchange(other.fd_, -1))
    , size_(std::exchange(other.size_, 0))
    , map_(std::exchange(other.map_, nullptr))
    , exportable_(other.exportable_)
{
}

DmaBuffer& DmaBuffer::operator=(DmaBuffer&& other) noexcept
{
    if (this != &other) {
        this->~DmaBuffer();
        new (this) DmaBuffer(std::move(other));
    }
    return *this;
}

DmaBuffer::~DmaBuffer()
{
    if (map_) {
        munmap(map_, size_);
    }
    if (fd_ >= 0) {
        close(fd_);
    }
}

int DmaBuffer::duplicate_fd() const
{
    if (!exportable_) {
        throw std::runtime_error("Converted picture is not in a DMA-BUF: the kernel offers neither dma-heap nor "
                                 "udmabuf (CONFIG_DMABUF_HEAPS_SYSTEM / CONFIG_UDMABUF), or access to "
                                 "/dev/dma_heap/system or /dev/udmabuf is denied");
    }
    const int result = fcntl(fd_, F_DUPFD_CLOEXEC, 0);
    if (result < 0) {
        throw std::system_error(errno, std::generic_category(), "Unable to duplicate DMA-BUF descriptor");
    }
    return result;
}

void DmaBuffer::begin_cpu_access(bool write) const
{
    if (!exportable_) {
        return;
    }
    sync_cpu_access(fd_, DMA_BUF_SYNC_START | (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ));
}

void DmaBuffer::end_cpu_access(bool write) const
{
    if (!exportable_) {
        return;
    }
    sync_cpu_access(fd_, DMA_BUF_SYNC_END | (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ));
}

void DmaBuffer::begin_cpu_access_rw() const
{
    if (!exportable_) {
        return;
    }
    sync_cpu_access(fd_, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
}

void DmaBuffer::end_cpu_access_rw() const
{
    if (!exportable_) {
        return;
    }
    sync_cpu_access(fd_, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
}
