/*
 * Copyright (C) 2007 Intel Corporation
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2023 Max Schettler <max.schettler@posteo.de>
 *
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

#include "buffer.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>

extern "C" {
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <va/va.h>
#include <va/va_drmcommon.h>
}

#include "driver.h"
#include "utils.h"
#include "v4l2.h"

Buffer::Buffer(VABufferType type, unsigned count, unsigned size, VASurfaceID derived_surface_id = VA_INVALID_ID)
    : type(type)
    , count(count)
    , data(type == VAEncCodedBufferType ? nullptr : static_cast<uint8_t*>(calloc(size, count)), BufferDeleter())
    , size(size)
    , derived_surface_id(derived_surface_id)
    , info({ .handle = static_cast<uintptr_t>(-1) })
    , coded_data(type == VAEncCodedBufferType ? static_cast<size_t>(size) * count : 0)
{
    if (type == VAEncCodedBufferType) {
        coded_segment.buf = coded_data.data();
    }
}

Buffer::Buffer(VABufferType type, unsigned count, unsigned size, uint8_t* external_data,
    std::shared_ptr<void> keep_alive, VASurfaceID derived_surface_id, std::shared_ptr<DmaBuffer> sync_buffer)
    : type(type)
    , count(count)
    , data(external_data, BufferDeleter { std::move(keep_alive) })
    , size(size)
    , derived_surface_id(derived_surface_id)
    , info({ .handle = static_cast<uintptr_t>(-1) })
    , sync_buffer(std::move(sync_buffer))
{
}

VAStatus createExternalBuffer(VADriverContextP context, VABufferType type, unsigned int size, uint8_t* data,
    std::shared_ptr<void> keep_alive, VASurfaceID derived_surface_id, VABufferID* buffer_id,
    std::shared_ptr<DmaBuffer> sync_buffer)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    std::lock_guard<std::mutex> guard(driver_data->mutex);
    *buffer_id = smallest_free_key(driver_data->buffers);
    auto [buffer, inserted]
        = driver_data->buffers.emplace(std::make_pair(*buffer_id,
            Buffer(type, 1, size, data, std::move(keep_alive), derived_surface_id, std::move(sync_buffer))));
    return inserted ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_ALLOCATION_FAILED;
}

VAStatus createBuffer(VADriverContextP context, VAContextID context_id, VABufferType type, unsigned int size,
    unsigned int count, void* data, VABufferID* buffer_id)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    switch (type) {
    case VAPictureParameterBufferType:
    case VAIQMatrixBufferType:
    case VASliceParameterBufferType:
    case VASliceDataBufferType:
    case VAImageBufferType:
    case VAProbabilityBufferType:
    case VAEncCodedBufferType:
    case VAEncSequenceParameterBufferType:
    case VAEncPictureParameterBufferType:
    case VAEncSliceParameterBufferType:
    case VAEncPackedHeaderParameterBufferType:
    case VAEncPackedHeaderDataBufferType:
    case VAEncMiscParameterBufferType:
    case VAEncMacroblockParameterBufferType:
    case VAEncMacroblockMapBufferType:
    case VAEncQPBufferType:
        break;

    default:
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    }

    if (type == VAEncCodedBufferType && data) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    std::lock_guard<std::mutex> guard(driver_data->mutex);
    *buffer_id = smallest_free_key(driver_data->buffers);
    auto [buffer, inserted] = driver_data->buffers.emplace(std::make_pair(*buffer_id, Buffer(type, count, size)));
    if (!inserted || (!buffer->second.data && type != VAEncCodedBufferType)) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    if (data) {
        std::copy_n(static_cast<uint8_t*>(data), size * count, buffer->second.data.get());
    }

    return VA_STATUS_SUCCESS;
}

VAStatus destroyBuffer(VADriverContextP context, VABufferID buffer_id)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    std::lock_guard<std::mutex> guard(driver_data->mutex);
    auto buffer_it = driver_data->buffers.find(buffer_id);
    if (buffer_it == driver_data->buffers.end()) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    driver_data->buffers.erase(buffer_it);

    return VA_STATUS_SUCCESS;
}

VAStatus mapBuffer(VADriverContextP context, VABufferID buffer_id, void** data_map)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    if (!driver_data->buffers.contains(buffer_id)) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    /* Our buffers are always mapped. */
    auto& buffer = driver_data->buffers.at(buffer_id);
    if (buffer.sync_buffer && !buffer.cpu_write_mapped) {
        try {
            buffer.sync_buffer->begin_cpu_access_rw();
            buffer.cpu_write_mapped = true;
        } catch (const std::exception& e) {
            error_log(context, "Unable to begin CPU access to surface buffer: %s\n", e.what());
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }
    *data_map = (buffer.type == VAEncCodedBufferType)
        ? static_cast<void*>(&buffer.coded_segment) : static_cast<void*>(buffer.data.get());

    return VA_STATUS_SUCCESS;
}

VAStatus unmapBuffer(VADriverContextP context, VABufferID buffer_id)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    /* Our buffers are always mapped. */
    if (!driver_data->buffers.contains(buffer_id)) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    auto& buffer = driver_data->buffers.at(buffer_id);
    if (buffer.sync_buffer && buffer.cpu_write_mapped) {
        try {
            buffer.sync_buffer->end_cpu_access_rw();
            buffer.cpu_write_mapped = false;
        } catch (const std::exception& e) {
            error_log(context, "Unable to end CPU access to surface buffer: %s\n", e.what());
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    return VA_STATUS_SUCCESS;
}

VAStatus bufferSetNumElements(VADriverContextP context, VABufferID buffer_id, unsigned int count)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    if (!driver_data->buffers.contains(buffer_id)) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    auto& buffer = driver_data->buffers.at(buffer_id);

    if (buffer.type == VAEncCodedBufferType) {
        if (buffer.data.get_deleter().keep_alive) {
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        buffer.coded_data.resize(static_cast<size_t>(buffer.size) * count);
        buffer.count = count;
        buffer.coded_segment = {};
        buffer.coded_segment.buf = buffer.coded_data.data();
        return VA_STATUS_SUCCESS;
    }

    if (buffer.data.get_deleter().keep_alive) {
        return VA_STATUS_ERROR_INVALID_BUFFER; // memory of someone else, can't be resized
    }
    buffer.data.reset(static_cast<uint8_t*>(reallocarray(buffer.data.release(), buffer.size, count)));
    buffer.count = count;

    return VA_STATUS_SUCCESS;
}

VAStatus bufferInfo(
    VADriverContextP context, VABufferID buffer_id, VABufferType* type, unsigned int* size, unsigned int* count)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    if (!driver_data->buffers.contains(buffer_id)) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    auto& buffer = driver_data->buffers.at(buffer_id);

    *type = buffer.type;
    *size = buffer.size;
    *count = buffer.count;

    return VA_STATUS_SUCCESS;
}

VAStatus acquireBufferHandle(VADriverContextP context, VABufferID buffer_id, VABufferInfo* buffer_info)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus releaseBufferHandle(VADriverContextP context, VABufferID buffer_id)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}
