/*
 * PROJECT:     LiberNT Kernel-Mode Driver Framework
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     USB host Controller eXtension (UCX) class library -- URB shape
 *              check (see ucxurbcheck.h). One table row per URB function this
 *              library carries: the bytes the function needs, and a rule for
 *              its fields. Everything else is refused before a dispatcher
 *              reads a byte beyond the header.
 * NOTE:        This file has no kernel dependencies beyond the USB headers, so
 *              the host test builds it as is (UCX_URBCHECK_HOST).
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include "ucxurbcheck.h"

/* A transfer of Length bytes needs somewhere to put them */
static
BOOLEAN
UcxpUrbBufferPresent(
    _In_ ULONG Length,
    _In_opt_ const VOID *Buffer,
    _In_opt_ const VOID *Mdl)
{
    return (Length == 0) || (Buffer != NULL) || (Mdl != NULL);
}

/* One USBD_INTERFACE_INFORMATION inside Remaining bytes of the URB */
static
USBD_STATUS
UcxpUrbCheckInterfaceInformation(
    _In_ const USBD_INTERFACE_INFORMATION *Info,
    _In_ ULONG Remaining,
    _In_ const UCX_URB_LIMITS *Limits,
    _Out_ PULONG Consumed)
{
    ULONG Needed;

    *Consumed = 0;
    if (Remaining < FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes))
        return USBD_STATUS_INVALID_PARAMETER;
    if (Info->NumberOfPipes > Limits->MaxPipesPerInterface)
        return USBD_STATUS_INVALID_PARAMETER;
    Needed = FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes) + Info->NumberOfPipes * sizeof(USBD_PIPE_INFORMATION);
    if (Info->Length < Needed || Info->Length > Remaining)
        return USBD_STATUS_INVALID_PARAMETER;
    *Consumed = Info->Length;
    return USBD_STATUS_SUCCESS;
}

USBD_STATUS
UcxUrbCheck(
    _In_reads_bytes_(Urb->UrbHeader.Length) const URB *Urb,
    _In_ const UCX_URB_LIMITS *Limits)
{
    USHORT Length = Urb->UrbHeader.Length;
    USHORT Function = Urb->UrbHeader.Function;

    if (Length < sizeof(struct _URB_HEADER))
        return USBD_STATUS_INVALID_PARAMETER;

    switch (Function)
    {
        /* ---- configuration ----------------------------------------------- */
        case URB_FUNCTION_SELECT_CONFIGURATION:
        {
            const struct _URB_SELECT_CONFIGURATION *Req = &Urb->UrbSelectConfiguration;
            const USBD_INTERFACE_INFORMATION *Info;
            ULONG Remaining, Count = 0, Consumed;

            if (Length < FIELD_OFFSET(struct _URB_SELECT_CONFIGURATION, Interface))
                return USBD_STATUS_INVALID_PARAMETER;
            if (Req->ConfigurationDescriptor == NULL)
                return USBD_STATUS_SUCCESS;                /* unconfigure: no interfaces */
            /* The interface entries fill the rest of the URB, each inside it */
            Info = &Req->Interface;
            Remaining = Length - FIELD_OFFSET(struct _URB_SELECT_CONFIGURATION, Interface);
            while (Remaining >= FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes))
            {
                USBD_STATUS UsbdStatus = UcxpUrbCheckInterfaceInformation(Info, Remaining, Limits, &Consumed);

                if (!USBD_SUCCESS(UsbdStatus))
                    return UsbdStatus;
                if (++Count > Limits->MaxInterfaces)
                    return USBD_STATUS_INVALID_PARAMETER;
                Remaining -= Consumed;
                Info = (const USBD_INTERFACE_INFORMATION *)((const UCHAR *)Info + Consumed);
            }
            return (Count != 0) ? USBD_STATUS_SUCCESS : USBD_STATUS_INVALID_PARAMETER;
        }

        case URB_FUNCTION_SELECT_INTERFACE:
        {
            const struct _URB_SELECT_INTERFACE *Req = &Urb->UrbSelectInterface;
            ULONG Consumed;

            if (Length < FIELD_OFFSET(struct _URB_SELECT_INTERFACE, Interface))
                return USBD_STATUS_INVALID_PARAMETER;
            if (Req->ConfigurationHandle == NULL)
                return USBD_STATUS_INVALID_PARAMETER;
            return UcxpUrbCheckInterfaceInformation(&Req->Interface,
                                                    Length - FIELD_OFFSET(struct _URB_SELECT_INTERFACE, Interface),
                                                    Limits, &Consumed);
        }

        /* ---- pipes --------------------------------------------------------- */
        case URB_FUNCTION_ABORT_PIPE:
        case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
        case URB_FUNCTION_SYNC_RESET_PIPE:
        case URB_FUNCTION_SYNC_CLEAR_STALL:
            if (Length < sizeof(struct _URB_PIPE_REQUEST))
                return USBD_STATUS_INVALID_PARAMETER;
            if (Urb->UrbPipeRequest.PipeHandle == NULL)
                return USBD_STATUS_INVALID_PIPE_HANDLE;
            return USBD_STATUS_SUCCESS;

        case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
        {
            const struct _URB_BULK_OR_INTERRUPT_TRANSFER *Req = &Urb->UrbBulkOrInterruptTransfer;

            if (Length < sizeof(*Req))
                return USBD_STATUS_INVALID_PARAMETER;
            if (Req->PipeHandle == NULL)
                return USBD_STATUS_INVALID_PIPE_HANDLE;
            if (Req->TransferBufferLength > Limits->MaxBulkTransfer ||
                !UcxpUrbBufferPresent(Req->TransferBufferLength, Req->TransferBuffer, Req->TransferBufferMDL))
                return USBD_STATUS_INVALID_PARAMETER;
            return USBD_STATUS_SUCCESS;
        }

        case URB_FUNCTION_ISOCH_TRANSFER:
        {
            const struct _URB_ISOCH_TRANSFER *Req = &Urb->UrbIsochronousTransfer;

            if (Length < FIELD_OFFSET(struct _URB_ISOCH_TRANSFER, IsoPacket))
                return USBD_STATUS_INVALID_PARAMETER;
            if (Req->PipeHandle == NULL)
                return USBD_STATUS_INVALID_PIPE_HANDLE;
            if (Req->NumberOfPackets == 0 || Req->NumberOfPackets > Limits->MaxIsochPackets ||
                Length < FIELD_OFFSET(struct _URB_ISOCH_TRANSFER, IsoPacket) +
                         Req->NumberOfPackets * sizeof(USBD_ISO_PACKET_DESCRIPTOR) ||
                !UcxpUrbBufferPresent(Req->TransferBufferLength, Req->TransferBuffer, Req->TransferBufferMDL))
                return USBD_STATUS_INVALID_PARAMETER;
            return USBD_STATUS_SUCCESS;
        }

        /* ---- control requests ----------------------------------------------- */
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT:
        case URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE:
        case URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE:
        case URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT:
        {
            const struct _URB_CONTROL_DESCRIPTOR_REQUEST *Req = &Urb->UrbControlDescriptorRequest;

            if (Length < sizeof(*Req))
                return USBD_STATUS_INVALID_PARAMETER;
            if (Req->TransferBufferLength > Limits->MaxControlTransfer ||
                !UcxpUrbBufferPresent(Req->TransferBufferLength, Req->TransferBuffer, Req->TransferBufferMDL))
                return USBD_STATUS_INVALID_PARAMETER;
            return USBD_STATUS_SUCCESS;
        }

        case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
        case URB_FUNCTION_GET_STATUS_FROM_INTERFACE:
        case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:
        case URB_FUNCTION_GET_STATUS_FROM_OTHER:
        {
            const struct _URB_CONTROL_GET_STATUS_REQUEST *Req = &Urb->UrbControlGetStatusRequest;

            if (Length < sizeof(*Req))
                return USBD_STATUS_INVALID_PARAMETER;
            if (Req->TransferBufferLength != sizeof(USHORT) ||
                !UcxpUrbBufferPresent(Req->TransferBufferLength, Req->TransferBuffer, Req->TransferBufferMDL))
                return USBD_STATUS_INVALID_PARAMETER;
            return USBD_STATUS_SUCCESS;
        }

        case URB_FUNCTION_SET_FEATURE_TO_DEVICE:
        case URB_FUNCTION_SET_FEATURE_TO_INTERFACE:
        case URB_FUNCTION_SET_FEATURE_TO_ENDPOINT:
        case URB_FUNCTION_SET_FEATURE_TO_OTHER:
        case URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE:
        case URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE:
        case URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT:
        case URB_FUNCTION_CLEAR_FEATURE_TO_OTHER:
            if (Length < sizeof(struct _URB_CONTROL_FEATURE_REQUEST))
                return USBD_STATUS_INVALID_PARAMETER;
            return USBD_STATUS_SUCCESS;

        case URB_FUNCTION_CLASS_DEVICE:
        case URB_FUNCTION_CLASS_INTERFACE:
        case URB_FUNCTION_CLASS_ENDPOINT:
        case URB_FUNCTION_CLASS_OTHER:
        case URB_FUNCTION_VENDOR_DEVICE:
        case URB_FUNCTION_VENDOR_INTERFACE:
        case URB_FUNCTION_VENDOR_ENDPOINT:
        case URB_FUNCTION_VENDOR_OTHER:
        {
            const struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST *Req = &Urb->UrbControlVendorClassRequest;

            if (Length < sizeof(*Req))
                return USBD_STATUS_INVALID_PARAMETER;
            if (Req->TransferBufferLength > Limits->MaxControlTransfer ||
                !UcxpUrbBufferPresent(Req->TransferBufferLength, Req->TransferBuffer, Req->TransferBufferMDL))
                return USBD_STATUS_INVALID_PARAMETER;
            return USBD_STATUS_SUCCESS;
        }

        case URB_FUNCTION_GET_CONFIGURATION:
        {
            const struct _URB_CONTROL_GET_CONFIGURATION_REQUEST *Req = &Urb->UrbControlGetConfigurationRequest;

            if (Length < sizeof(*Req))
                return USBD_STATUS_INVALID_PARAMETER;
            if (Req->TransferBufferLength != 1 ||
                !UcxpUrbBufferPresent(Req->TransferBufferLength, Req->TransferBuffer, Req->TransferBufferMDL))
                return USBD_STATUS_INVALID_PARAMETER;
            return USBD_STATUS_SUCCESS;
        }

        case URB_FUNCTION_GET_INTERFACE:
        {
            const struct _URB_CONTROL_GET_INTERFACE_REQUEST *Req = &Urb->UrbControlGetInterfaceRequest;

            if (Length < sizeof(*Req))
                return USBD_STATUS_INVALID_PARAMETER;
            if (Req->TransferBufferLength != 1 ||
                !UcxpUrbBufferPresent(Req->TransferBufferLength, Req->TransferBuffer, Req->TransferBufferMDL))
                return USBD_STATUS_INVALID_PARAMETER;
            return USBD_STATUS_SUCCESS;
        }

        case URB_FUNCTION_CONTROL_TRANSFER:
        {
            const struct _URB_CONTROL_TRANSFER *Req = &Urb->UrbControlTransfer;
            const USB_DEFAULT_PIPE_SETUP_PACKET *Setup = (const USB_DEFAULT_PIPE_SETUP_PACKET *)Req->SetupPacket;

            if (Length < sizeof(*Req))
                return USBD_STATUS_INVALID_PARAMETER;
            /* The device may send up to wLength bytes: the buffer must hold them */
            if (Setup->wLength > Req->TransferBufferLength ||
                Req->TransferBufferLength > Limits->MaxControlTransfer ||
                !UcxpUrbBufferPresent(Req->TransferBufferLength, Req->TransferBuffer, Req->TransferBufferMDL))
                return USBD_STATUS_INVALID_PARAMETER;
            return USBD_STATUS_SUCCESS;
        }

        default:
            return USBD_STATUS_INVALID_URB_FUNCTION;
    }
}

const char *
UcxUrbFunctionName(
    _In_ USHORT Function)
{
    switch (Function)
    {
        case URB_FUNCTION_SELECT_CONFIGURATION:            return "SELECT_CONFIGURATION";
        case URB_FUNCTION_SELECT_INTERFACE:                return "SELECT_INTERFACE";
        case URB_FUNCTION_ABORT_PIPE:                      return "ABORT_PIPE";
        case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL: return "SYNC_RESET_PIPE_AND_CLEAR_STALL";
        case URB_FUNCTION_SYNC_RESET_PIPE:                 return "SYNC_RESET_PIPE";
        case URB_FUNCTION_SYNC_CLEAR_STALL:                return "SYNC_CLEAR_STALL";
        case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:      return "BULK_OR_INTERRUPT_TRANSFER";
        case URB_FUNCTION_ISOCH_TRANSFER:                  return "ISOCH_TRANSFER";
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:      return "GET_DESCRIPTOR_FROM_DEVICE";
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:   return "GET_DESCRIPTOR_FROM_INTERFACE";
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT:    return "GET_DESCRIPTOR_FROM_ENDPOINT";
        case URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE:        return "SET_DESCRIPTOR_TO_DEVICE";
        case URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE:     return "SET_DESCRIPTOR_TO_INTERFACE";
        case URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT:      return "SET_DESCRIPTOR_TO_ENDPOINT";
        case URB_FUNCTION_GET_STATUS_FROM_DEVICE:          return "GET_STATUS_FROM_DEVICE";
        case URB_FUNCTION_GET_STATUS_FROM_INTERFACE:       return "GET_STATUS_FROM_INTERFACE";
        case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:        return "GET_STATUS_FROM_ENDPOINT";
        case URB_FUNCTION_GET_STATUS_FROM_OTHER:           return "GET_STATUS_FROM_OTHER";
        case URB_FUNCTION_SET_FEATURE_TO_DEVICE:           return "SET_FEATURE_TO_DEVICE";
        case URB_FUNCTION_SET_FEATURE_TO_INTERFACE:        return "SET_FEATURE_TO_INTERFACE";
        case URB_FUNCTION_SET_FEATURE_TO_ENDPOINT:         return "SET_FEATURE_TO_ENDPOINT";
        case URB_FUNCTION_SET_FEATURE_TO_OTHER:            return "SET_FEATURE_TO_OTHER";
        case URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE:         return "CLEAR_FEATURE_TO_DEVICE";
        case URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE:      return "CLEAR_FEATURE_TO_INTERFACE";
        case URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT:       return "CLEAR_FEATURE_TO_ENDPOINT";
        case URB_FUNCTION_CLEAR_FEATURE_TO_OTHER:          return "CLEAR_FEATURE_TO_OTHER";
        case URB_FUNCTION_CLASS_DEVICE:                    return "CLASS_DEVICE";
        case URB_FUNCTION_CLASS_INTERFACE:                 return "CLASS_INTERFACE";
        case URB_FUNCTION_CLASS_ENDPOINT:                  return "CLASS_ENDPOINT";
        case URB_FUNCTION_CLASS_OTHER:                     return "CLASS_OTHER";
        case URB_FUNCTION_VENDOR_DEVICE:                   return "VENDOR_DEVICE";
        case URB_FUNCTION_VENDOR_INTERFACE:                return "VENDOR_INTERFACE";
        case URB_FUNCTION_VENDOR_ENDPOINT:                 return "VENDOR_ENDPOINT";
        case URB_FUNCTION_VENDOR_OTHER:                    return "VENDOR_OTHER";
        case URB_FUNCTION_GET_CONFIGURATION:               return "GET_CONFIGURATION";
        case URB_FUNCTION_GET_INTERFACE:                   return "GET_INTERFACE";
        case URB_FUNCTION_CONTROL_TRANSFER:                return "CONTROL_TRANSFER";
        default:                                           return "?";
    }
}
