#include "precomp.h"
#include "vioser.h"
#include "public.h"

#if defined(EVENT_TRACING)
#include "Control.tmh"
#endif

static
VOID
VIOSerialHandleCtrlMsg(
    IN WDFDEVICE Device,
    IN PPORT_BUFFER buf
);

VOID
VIOSerialSendCtrlMsg(
    IN WDFDEVICE Device,
    IN ULONG id,
    IN USHORT event,
    IN USHORT value
)
{
    struct VirtIOBufferDescriptor sg;
    struct virtqueue *vq;
    UINT len;
    PPORTS_DEVICE pContext = GetPortsDevice(Device);
    VIRTIO_CONSOLE_CONTROL cpkt;
    int cnt = 0;
    if (!pContext->isHostMultiport)
    {
        return;
    }

    vq = pContext->c_ovq;

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "--> %s vq = %p\n", __FUNCTION__, vq);

    cpkt.id = id;
    cpkt.event = event;
    cpkt.value = value;

    sg.physAddr = MmGetPhysicalAddress(&cpkt);
    sg.ulSize = sizeof(cpkt);

    if(0 <= vq->vq_ops->add_buf(vq, &sg, 1, 0, &cpkt, NULL, 0))
    {
        vq->vq_ops->kick(vq);
        while(!vq->vq_ops->get_buf(vq, &len))
        {
           KeStallExecutionProcessor(50);
           if(++cnt > RETRY_THRESHOLD)
           {
              TraceEvents(TRACE_LEVEL_FATAL, DBG_PNP, "<-> %s retries = %d\n", __FUNCTION__, cnt);
              break;
           }
        }
    }
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "<-- %s cnt = %d\n", __FUNCTION__, cnt);
}

VOID
VIOSerialCtrlWorkHandler(
    IN WDFDEVICE Device
)
{
    struct virtqueue *vq;
    PPORT_BUFFER buf;
    UINT len;
    NTSTATUS  status = STATUS_SUCCESS;
    PPORTS_DEVICE pContext = GetPortsDevice(Device);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_PNP, "--> %s\n", __FUNCTION__);

    vq = pContext->c_ivq;
    ASSERT(vq);

    WdfSpinLockAcquire(pContext->CVqLock);
    while ((buf = vq->vq_ops->get_buf(vq, &len)))
    {
        WdfSpinLockRelease(pContext->CVqLock);
        buf->len = len;
        buf->offset = 0;
        VIOSerialHandleCtrlMsg(Device, buf);

        WdfSpinLockAcquire(pContext->CVqLock);
        status = VIOSerialAddInBuf(vq, buf);
        if (!NT_SUCCESS(status))
        {
           TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "%s::%d Error adding buffer to queue\n", __FUNCTION__, __LINE__);
           VIOSerialFreeBuffer(buf);
        }
    }
    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_PNP, "<-- %s\n", __FUNCTION__);
    WdfSpinLockRelease(pContext->CVqLock);
}

VOID
VIOSerialHandleCtrlMsg(
    IN WDFDEVICE Device,
    IN PPORT_BUFFER buf
)
{
    PPORTS_DEVICE pContext = GetPortsDevice(Device);
    PVIRTIO_CONSOLE_CONTROL cpkt;
    PVIOSERIAL_PORT port;

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "--> %s\n", __FUNCTION__);

    cpkt = (PVIRTIO_CONSOLE_CONTROL)((ULONG_PTR)buf->va_buf + buf->offset);

    port = VIOSerialFindPortById(Device, cpkt->id);

    if (!port && (cpkt->event != VIRTIO_CONSOLE_PORT_ADD))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "Invlid index %u in control packet\n", cpkt->id);
    }

    switch (cpkt->event)
    {
        case VIRTIO_CONSOLE_PORT_ADD:
           if (port)
           {
               TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "VIRTIO_CONSOLE_PORT_ADD id = %d\n", cpkt->id);
               break;
           }
           if (cpkt->id >= pContext->consoleConfig.max_nr_ports)
           {
               TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "Out-of-bound id %u\n", cpkt->id);
               break;
           }
           VIOSerialAddPort(Device, cpkt->id);
        break;

        case VIRTIO_CONSOLE_PORT_REMOVE:
           if (!port)
           {
              TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "VIRTIO_CONSOLE_PORT_REMOVE invalid id = %d\n", cpkt->id);
              break;
           }
           TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "VIRTIO_CONSOLE_PORT_REMOVE id = %d\n", cpkt->id);
           VIOSerialRemovePort(Device, port);
        break;

        case VIRTIO_CONSOLE_CONSOLE_PORT:
           TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
                       "VIRTIO_CONSOLE_CONSOLE_PORT id = %d value = %u\n", cpkt->id, cpkt->value);
           if (cpkt->value)
           {
              VIOSerialInitPortConsole(port);
           }
        break;

        case VIRTIO_CONSOLE_RESIZE:
           TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "VIRTIO_CONSOLE_RESIZE id = %d\n", cpkt->id);
        break;

        case VIRTIO_CONSOLE_PORT_OPEN:
           if (port)
           {
              BOOLEAN  Connected = (BOOLEAN)cpkt->value;
              TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "VIRTIO_CONSOLE_PORT_OPEN id = %d, HostConnected = %d\n", cpkt->id, Connected);
              if (port->HostConnected != Connected)
              {
                 VIOSerialPortPnpNotify(Device, port, Connected);
              }
           }
           else
           {
              TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "VIRTIO_CONSOLE_PORT_OPEN invalid id = %d\n", cpkt->id);
           }
        break;

        case VIRTIO_CONSOLE_PORT_NAME:
           if (port)
           {
              VIOSerialPortCreateName(Device, port, buf);
           }
        break;
        default:
           TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "%s UNKNOWN event = %d\n", __FUNCTION__, cpkt->event);
    }
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "<-- %s\n", __FUNCTION__);
}
