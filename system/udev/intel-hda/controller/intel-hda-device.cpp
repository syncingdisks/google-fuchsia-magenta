// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <intel-hda-driver-utils/driver-channel.h>
#include <intel-hda-driver-utils/intel-hda-proto.h>
#include <mx/handle.h>
#include <mxtl/type_support.h>

#include "intel-hda-codec.h"
#include "intel-hda-controller.h"
#include "intel-hda-device.h"

template <typename DeviceType>
ssize_t IntelHDADevice<DeviceType>::DeviceIoctl(uint32_t op,
                                                const void* in_buf, size_t in_len,
                                                void* out_buf, size_t out_len) {
    if ((op != IHDA_IOCTL_GET_CHANNEL) ||
        (out_buf == nullptr) ||
        (out_len != sizeof(mx_handle_t)))
        return ERR_INVALID_ARGS;

    auto channel = DriverChannelAllocator::New();
    if (channel == nullptr)
        return ERR_NO_MEMORY;

    mx::channel out_channel;
    mx_status_t res = channel->Activate(mxtl::WrapRefPtr(this), &out_channel);
    *(reinterpret_cast<mx_handle_t*>(out_buf)) = out_channel.release();

    return res;
}

template <typename DeviceType>
void IntelHDADevice<DeviceType>::Shutdown() {
    // Prevent new callbacks from starting and synchronize with callbacks in flight.
    {
        mxtl::AutoLock process_lock(&process_lock_);
        if (is_shutdown_)
            return;

        is_shutdown_ = true;
    }

    // Shutdown all of our existing driver channels.
    DriverChannel::Owner::ShutdownDriverChannels();
}

template <typename DeviceType>
mx_status_t IntelHDADevice<DeviceType>::ProcessChannel(DriverChannel& channel,
                                                       const mx_io_packet_t& io_packet) {
    using RequestBufferType = typename DeviceType::RequestBufferType;

    // TODO(johngro) : How large is too large?
    static_assert(sizeof(RequestBufferType) <= 256,
                  "Request buffer is getting to be too large to hold on the stack!");

    // Read the request from the channel; note that the thread pool *should* be
    // serializing access to the ports on a per-channel basis, so there should
    // be no possibility of message re-ordering on a given channel.
    RequestBufferType request_buffer;
    uint32_t    bytes;
    mx::handle  handle;
    mx_status_t res = channel.Read(&request_buffer, sizeof(request_buffer), &bytes, &handle);

    if (res != NO_ERROR) {
        MX_DEBUG_ASSERT(handle == MX_HANDLE_INVALID);
        return res;
    }

    // Enter the process lock and attempt to dispatch the request.  If the
    // is_shutdown_ flag has been set, just abort.  No need to propagate an
    // error, the channel is already being shutdown.
    {
        mxtl::AutoLock process_lock(&process_lock_);
        if (!is_shutdown_) {
            // Downcast ourselves and process the request.
            static_assert(mxtl::is_base_of<IntelHDADevice<DeviceType>, DeviceType>::value,
                          "DeviceType must derrive from IntelHDADevice<DeviceType>");

            res = static_cast<DeviceType*>(this)->ProcessClientRequest(channel,
                                                                       request_buffer,
                                                                       bytes,
                                                                       mxtl::move(handle));
        }
    }

    return res;
}

template class IntelHDADevice<IntelHDACodec>;
template class IntelHDADevice<IntelHDAController>;