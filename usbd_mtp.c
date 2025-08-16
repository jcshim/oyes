/**
  ******************************************************************************
  * @file    usbd_mtp.c (re-patched, 2025-08-16)
  * @brief   MTP/PTP class: robust writes + correct handle filtering.
  *          - Larger TX buffer (16KB) for big GetObjectHandles results.
  *          - Proper GetObjectHandles/NumObjects param parsing (storage, format, parent).
  *          - Safe multi-packet SendObjectInfo handling + payload streaming for SendObject.
  *          - Header accumulator for short OUT packets (>=12B).
  *          - Filename UTF-16LE decode & sanitization kept minimal in if.c (source of truth).
  ******************************************************************************
  */

#include "usbd_core.h"
#include "usbd_ctlreq.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

/* ---------- Config (Endpoint numbers must match descriptors) ---------- */
#ifndef MTP_IN_EP
#define MTP_IN_EP     0x81  /* Bulk IN  */
#endif
#ifndef MTP_OUT_EP
#define MTP_OUT_EP    0x01  /* Bulk OUT */
#endif
#ifndef MTP_EVT_EP
#define MTP_EVT_EP    0x83  /* Interrupt IN (events) */
#endif

#define USB_MTP_CONFIG_DESC_SIZ   (9+9+7+7+7)
#define MTP_DATA_MAX_PACKET_SIZE  64U
#define MTP_EVT_PACKET_SIZE       16U

/* Buffers */
#define MTP_RX_BUFSZ  1024U
#define MTP_TX_BUFSZ  16384U

/* PTP/MTP constants */
#define PTP_CONTAINER_UNDEFINED   0
#define PTP_CONTAINER_COMMAND     1
#define PTP_CONTAINER_DATA        2
#define PTP_CONTAINER_RESPONSE    3
#define PTP_CONTAINER_EVENT       4

#define PTP_RC_OK                 0x2001
#define PTP_RC_GENERAL_ERROR      0x2002
#define PTP_RC_OPERATION_NOT_SUPPORTED 0x2005
#define PTP_RC_INVALID_PARAMETER  0x201D
#define PTP_RC_STORE_FULL         0x2003

#define PTP_OC_GetDeviceInfo      0x1001
#define PTP_OC_OpenSession        0x1002
#define PTP_OC_CloseSession       0x1003
#define PTP_OC_GetStorageIDs      0x1004
#define PTP_OC_GetStorageInfo     0x1005
#define PTP_OC_GetNumObjects      0x1006
#define PTP_OC_GetObjectHandles   0x1007
#define PTP_OC_GetObjectInfo      0x1008
#define PTP_OC_GetObject          0x1009
#define PTP_OC_DeleteObject       0x100B
#define PTP_OC_SendObjectInfo     0x100C
#define PTP_OC_SendObject         0x100D

/* Optional user interface (delegation to if.c) */
typedef struct
{
  uint32_t (*GetDeviceInfo)(uint8_t *buf, uint32_t max);
  int      (*OpenSession)(uint32_t id);
  int      (*CloseSession)(void);
  uint32_t (*GetStorageIDs)(uint8_t *buf, uint32_t max);
  uint32_t (*GetStorageInfo)(uint8_t *buf, uint32_t max, uint32_t storage_id);

  /* Updated: filter-aware list/count */
  uint32_t (*GetObjectHandles)(uint8_t *buf, uint32_t max,
                               uint32_t storage_id, uint16_t format, uint32_t parent);
  uint32_t (*CountObjects)(uint32_t storage_id, uint16_t format, uint32_t parent);

  uint32_t (*GetObjectInfo)(uint8_t *buf, uint32_t max, uint32_t handle);
  int      (*ObjectOpenRead)(uint32_t handle, uint32_t *size_bytes);
  int      (*ObjectReadChunk)(uint8_t *buf, uint32_t offs, uint32_t *len_inout);
  void     (*ObjectCloseRead)(void);

  int      (*ObjectBeginWrite)(uint32_t storage_id, uint32_t parent, const char *name,
                               uint32_t size_bytes, uint16_t format, uint32_t *out_handle);
  int      (*ObjectWriteChunk)(const uint8_t *data, uint32_t offs, uint32_t *len_inout);
  int      (*ObjectEndWrite)(int commit);
  int      (*DeleteObject)(uint32_t handle);
} USBD_MTP_ItfTypeDef;

static USBD_MTP_ItfTypeDef *MTP_fops = NULL;

uint8_t USBD_MTP_RegisterInterface(USBD_HandleTypeDef *pdev, USBD_MTP_ItfTypeDef *fops)
{
  (void)pdev;
  MTP_fops = fops;
  return (uint8_t)USBD_OK;
}

/* ---- helpers ---- */
static inline uint16_t rd16(const uint8_t *p){ return (uint16_t)(p[0] | (p[1]<<8)); }
static inline uint32_t rd32(const uint8_t *p){ return (uint32_t)(p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24)); }
static inline void wr16(uint8_t *p, uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static inline void wr32(uint8_t *p, uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }

/* Internal state */
typedef struct {
  uint32_t session_open;
  uint32_t transaction_id;

  uint8_t  rx_buf[MTP_RX_BUFSZ];
  uint8_t  tx_buf[MTP_TX_BUFSZ];

  /* header accumulator for short OUT packets */
  uint8_t  hdr_buf[12];
  uint32_t hdr_have;

  /* IN streaming for GetObject */
  uint8_t  in_streaming;
  uint32_t obj_size;
  uint32_t obj_offs;
  uint32_t obj_tid;

  /* OUT streaming for SendObject */
  enum { RX_IDLE=0, RX_WAIT_SI_DATA, RX_WAIT_OBJ_HDR, RX_RECV_OBJECT, RX_RECV_SI_MULTI } rx_state;
  uint32_t rx_tid;             /* current TID for op waiting */
  uint32_t data_total;         /* full container length (including 12-byte header) */
  uint32_t pay_total;          /* payload only (data_total - 12) */
  uint32_t pay_rcvd;           /* received payload bytes */
  uint16_t last_op;            /* op code for current RX */
  uint32_t write_offs;         /* running write offset for SendObject */

  /* Accumulator for SendObjectInfo DATA that arrives in multiple OUT packets */
  uint8_t  si_buf[2048];
  uint32_t si_need;
  uint32_t si_got;
} MTP_HandleTypeDef;

static MTP_HandleTypeDef hmtp;

/* ------------------------- Descriptors ------------------------------- */
__ALIGN_BEGIN static uint8_t USBD_MTP_FS_CfgDesc[] __ALIGN_END =
{
  /* Configuration Descriptor */
  0x09, USB_DESC_TYPE_CONFIGURATION,
  (uint8_t)(USB_MTP_CONFIG_DESC_SIZ & 0xFF), (uint8_t)(USB_MTP_CONFIG_DESC_SIZ >> 8),
  0x01, 0x01, 0x00, 0x80, 0x32,

  /* Interface Descriptor */
  0x09, USB_DESC_TYPE_INTERFACE,
  0x00, 0x00, 0x03, 0x06, 0x01, 0x01, 0x00,

  /* EP 0x81 Bulk IN */
  0x07, USB_DESC_TYPE_ENDPOINT, MTP_IN_EP, 0x02,
  LOBYTE(MTP_DATA_MAX_PACKET_SIZE), HIBYTE(MTP_DATA_MAX_PACKET_SIZE), 0x00,

  /* EP 0x01 Bulk OUT */
  0x07, USB_DESC_TYPE_ENDPOINT, MTP_OUT_EP, 0x02,
  LOBYTE(MTP_DATA_MAX_PACKET_SIZE), HIBYTE(MTP_DATA_MAX_PACKET_SIZE), 0x00,

  /* EP 0x83 Interrupt IN (MTP Event) */
  0x07, USB_DESC_TYPE_ENDPOINT, MTP_EVT_EP, 0x03,
  LOBYTE(MTP_EVT_PACKET_SIZE), HIBYTE(MTP_EVT_PACKET_SIZE), 0x06,
};

__ALIGN_BEGIN static uint8_t USBD_MTP_DeviceQualifierDesc[] __ALIGN_END =
{
  0x0A, USB_DESC_TYPE_DEVICE_QUALIFIER,
  0x00, 0x02,
  0x00, 0x00, 0x00,
  MTP_DATA_MAX_PACKET_SIZE,
  0x01, 0x00
};

/* --------------------- USBD class forward decls ---------------------- */
static uint8_t  USBD_MTP_Init(USBD_HandleTypeDef *pdev, uint8_t cfgidx);
static uint8_t  USBD_MTP_DeInit(USBD_HandleTypeDef *pdev, uint8_t cfgidx);
static uint8_t  USBD_MTP_Setup(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req);
static uint8_t  USBD_MTP_EP0_TxReady(USBD_HandleTypeDef *pdev);
static uint8_t  USBD_MTP_EP0_RxReady(USBD_HandleTypeDef *pdev);
static uint8_t  USBD_MTP_DataIn(USBD_HandleTypeDef *pdev, uint8_t epnum);
static uint8_t  USBD_MTP_DataOut(USBD_HandleTypeDef *pdev, uint8_t epnum);
static uint8_t  USBD_MTP_SOF(USBD_HandleTypeDef *pdev);
static uint8_t  USBD_MTP_IsoINIncomplete(USBD_HandleTypeDef *pdev, uint8_t epnum);
static uint8_t  USBD_MTP_IsoOUTIncomplete(USBD_HandleTypeDef *pdev, uint8_t epnum);
static uint8_t* USBD_MTP_GetFSConfigDescriptor(uint16_t *length);
static uint8_t* USBD_MTP_GetHSConfigDescriptor(uint16_t *length);
static uint8_t* USBD_MTP_GetOtherSpeedCfgDesc(uint16_t *length);
static uint8_t* USBD_MTP_GetDeviceQualifierDescriptor(uint16_t *length);

/* Class structure */
USBD_ClassTypeDef USBD_MTP =
{
  USBD_MTP_Init,
  USBD_MTP_DeInit,
  USBD_MTP_Setup,
  USBD_MTP_EP0_TxReady,
  USBD_MTP_EP0_RxReady,
  USBD_MTP_DataIn,
  USBD_MTP_DataOut,
  USBD_MTP_SOF,
  USBD_MTP_IsoINIncomplete,
  USBD_MTP_IsoOUTIncomplete,
  USBD_MTP_GetHSConfigDescriptor,
  USBD_MTP_GetFSConfigDescriptor,
  USBD_MTP_GetOtherSpeedCfgDesc,
  USBD_MTP_GetDeviceQualifierDescriptor
};

/* -------------------- Small helpers to send -------------------------- */

static void MTP_SendResponse(USBD_HandleTypeDef *pdev, uint16_t code, uint32_t tid)
{
  uint8_t *b = hmtp.tx_buf;
  wr32(b+0, 12);                 /* length */
  wr16(b+4, PTP_CONTAINER_RESPONSE);
  wr16(b+6, code);
  wr32(b+8, tid);
  USBD_LL_Transmit(pdev, MTP_IN_EP, b, 12U);
}

static void MTP_SendResponseParams(USBD_HandleTypeDef *pdev, uint16_t code, uint32_t tid,
                                   const uint32_t *params, uint32_t n_params)
{
  uint8_t *b = hmtp.tx_buf;
  uint32_t len = 12U + 4U * n_params;
  wr32(b+0, len);
  wr16(b+4, PTP_CONTAINER_RESPONSE);
  wr16(b+6, code);
  wr32(b+8, tid);
  for (uint32_t i=0;i<n_params;i++) wr32(b+12+4*i, params[i]);
  USBD_LL_Transmit(pdev, MTP_IN_EP, b, len);
}

/* Compose a data container entirely in tx_buf and transmit it in one go.
   NOTE: Ensure payload fits in tx_buf - 12. */
static void MTP_SendDataAndResponse(USBD_HandleTypeDef *pdev, uint16_t op, uint32_t tid,
                                    const uint8_t *payload, uint32_t paylen)
{
  uint8_t *b = hmtp.tx_buf;
  uint32_t room = (uint32_t)(sizeof(hmtp.tx_buf) - 12U);
  if (paylen > room) paylen = room; /* clamp */
  wr32(b+0, 12U + paylen);
  wr16(b+4, PTP_CONTAINER_DATA);
  wr16(b+6, op);
  wr32(b+8, tid);
  if (paylen && payload){ memcpy(b+12, payload, paylen); }
  USBD_LL_Transmit(pdev, MTP_IN_EP, b, 12U + paylen);
  /* When DataIn completes, send OK response. */
  hmtp.transaction_id = tid;
}

/* === Streaming for GetObject (IN) === */
static void MTP_StreamStart(USBD_HandleTypeDef *pdev, uint32_t tid, uint32_t total_size)
{
  hmtp.in_streaming = 1U;
  hmtp.obj_size  = total_size;
  hmtp.obj_offs  = 0U;
  hmtp.obj_tid   = tid;

  /* header + first chunk */
  uint8_t *b = hmtp.tx_buf;
  uint32_t room = (uint32_t)sizeof(hmtp.tx_buf) - 12U;
  uint32_t n = room;
  if (n > total_size) n = total_size;

  if (MTP_fops && MTP_fops->ObjectReadChunk){
    uint32_t want = n;
    if (MTP_fops->ObjectReadChunk(b+12, 0U, &want) != 0){
      hmtp.in_streaming = 0U;
      if (MTP_fops->ObjectCloseRead) MTP_fops->ObjectCloseRead();
      MTP_SendResponse(pdev, PTP_RC_GENERAL_ERROR, tid);
      return;
    }
    n = want;
  } else {
    MTP_SendResponse(pdev, PTP_RC_GENERAL_ERROR, tid);
    return;
  }

  wr32(b+0, 12U + total_size);
  wr16(b+4, PTP_CONTAINER_DATA);
  wr16(b+6, PTP_OC_GetObject);
  wr32(b+8, tid);
  USBD_LL_Transmit(pdev, MTP_IN_EP, b, 12U + n);
  hmtp.obj_offs = n;
}

/* ----------------------- Class callbacks ----------------------------- */

static uint8_t USBD_MTP_Init(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
  (void)cfgidx;
  memset(&hmtp, 0, sizeof(hmtp));

  USBD_LL_OpenEP(pdev, MTP_IN_EP,  USBD_EP_TYPE_BULK, MTP_DATA_MAX_PACKET_SIZE);
  USBD_LL_OpenEP(pdev, MTP_OUT_EP, USBD_EP_TYPE_BULK, MTP_DATA_MAX_PACKET_SIZE);
  USBD_LL_OpenEP(pdev, MTP_EVT_EP, USBD_EP_TYPE_INTR, MTP_EVT_PACKET_SIZE);

  pdev->ep_in[MTP_IN_EP & 0xFU].is_used   = 1U;
  pdev->ep_out[MTP_OUT_EP & 0xFU].is_used = 1U;
  pdev->ep_in[MTP_EVT_EP & 0xFU].is_used  = 1U;

  USBD_LL_PrepareReceive(pdev, MTP_OUT_EP, hmtp.rx_buf, sizeof(hmtp.rx_buf));
  return (uint8_t)USBD_OK;
}

static uint8_t USBD_MTP_DeInit(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
  (void)cfgidx;
  USBD_LL_CloseEP(pdev, MTP_IN_EP);
  USBD_LL_CloseEP(pdev, MTP_OUT_EP);
  USBD_LL_CloseEP(pdev, MTP_EVT_EP);

  pdev->ep_in[MTP_IN_EP & 0xFU].is_used   = 0U;
  pdev->ep_out[MTP_OUT_EP & 0xFU].is_used = 0U;
  pdev->ep_in[MTP_EVT_EP & 0xFU].is_used  = 0U;

  return (uint8_t)USBD_OK;
}

static uint8_t USBD_MTP_Setup(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
  if ((req->bmRequest & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_CLASS ||
      (req->bmRequest & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_VENDOR)
  {
    USBD_CtlError(pdev, req);
    return (uint8_t)USBD_FAIL;
  }
  return (uint8_t)USBD_OK;
}

static uint8_t USBD_MTP_EP0_TxReady(USBD_HandleTypeDef *pdev)
{ (void)pdev; return (uint8_t)USBD_OK; }

static uint8_t USBD_MTP_EP0_RxReady(USBD_HandleTypeDef *pdev)
{ (void)pdev; return (uint8_t)USBD_OK; }

static uint8_t USBD_MTP_DataIn(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
  (void)epnum;

  /* Continue streaming object data (IN: GetObject) */
  if (hmtp.in_streaming)
  {
    if (hmtp.obj_offs < hmtp.obj_size)
    {
      uint32_t remain = hmtp.obj_size - hmtp.obj_offs;
      uint32_t n = (remain > (uint32_t)sizeof(hmtp.tx_buf)) ? (uint32_t)sizeof(hmtp.tx_buf) : remain;

      if (MTP_fops && MTP_fops->ObjectReadChunk){
        uint32_t want = n;
        if (MTP_fops->ObjectReadChunk(hmtp.tx_buf, hmtp.obj_offs, &want) == 0 && want > 0U){
          USBD_LL_Transmit(pdev, MTP_IN_EP, hmtp.tx_buf, want);
          hmtp.obj_offs += want;
          return (uint8_t)USBD_OK;
        }
      }

      if (MTP_fops && MTP_fops->ObjectCloseRead) MTP_fops->ObjectCloseRead();
      hmtp.in_streaming = 0U;
      MTP_SendResponse(pdev, PTP_RC_GENERAL_ERROR, hmtp.obj_tid);
      return (uint8_t)USBD_OK;
    }

    if (MTP_fops && MTP_fops->ObjectCloseRead) MTP_fops->ObjectCloseRead();
    hmtp.in_streaming = 0U;
    MTP_SendResponse(pdev, PTP_RC_OK, hmtp.obj_tid);
    return (uint8_t)USBD_OK;
  }

  /* If previous op used MTP_SendDataAndResponse(), send OK response now */
  if (hmtp.transaction_id != 0U)
  {
    MTP_SendResponse(pdev, PTP_RC_OK, hmtp.transaction_id);
    hmtp.transaction_id = 0U;
  }
  return (uint8_t)USBD_OK;
}

static uint8_t USBD_MTP_DataOut(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
  (void)epnum;

  uint8_t *b = hmtp.rx_buf;
  uint32_t xfer = USBD_LL_GetRxDataSize(pdev, MTP_OUT_EP);

  /* === Ongoing payload for SendObject (no header) === */
  if (hmtp.rx_state == RX_RECV_OBJECT)
  {
    /* Append chunk */
    uint32_t chunk = xfer;
    if (hmtp.pay_rcvd + chunk > hmtp.pay_total) chunk = hmtp.pay_total - hmtp.pay_rcvd;

    if (MTP_fops && MTP_fops->ObjectWriteChunk){
      uint32_t want = chunk;
      if (MTP_fops->ObjectWriteChunk(b, hmtp.write_offs, &want) != 0){ /* write error → abort */
        if (MTP_fops->ObjectEndWrite) MTP_fops->ObjectEndWrite(0);
        hmtp.rx_state = RX_IDLE;
        MTP_SendResponse(pdev, PTP_RC_GENERAL_ERROR, hmtp.rx_tid);
        goto rearm;
      }
      hmtp.write_offs += want;
      hmtp.pay_rcvd   += want;
    }

    if (hmtp.pay_rcvd >= hmtp.pay_total){
      /* finished payload */
      if (MTP_fops && MTP_fops->ObjectEndWrite) MTP_fops->ObjectEndWrite(1);
      hmtp.rx_state = RX_IDLE;
      MTP_SendResponse(pdev, PTP_RC_OK, hmtp.rx_tid);
    }

    goto rearm;
  }

  /* === Accumulate header if needed === */
  if ((hmtp.rx_state == RX_WAIT_OBJ_HDR || hmtp.rx_state == RX_WAIT_SI_DATA) && (hmtp.hdr_have < 12U))
  {
    uint32_t need = 12U - hmtp.hdr_have;
    uint32_t take = (xfer > need) ? need : xfer;
    memcpy(hmtp.hdr_buf + hmtp.hdr_have, b, take);
    hmtp.hdr_have += take;

    if (take < xfer){
      /* move leftover (payload or rest of header) to start of rx_buf for uniform handling */
      memmove(b, b + take, xfer - take);
      xfer -= take;
    } else {
      xfer = 0;
    }

    if (hmtp.hdr_have < 12U) { goto rearm; }
    /* fallthrough: hdr_buf now ready; interpret using hdr_buf instead of b */
  }

  /* Interpret header from either hdr_buf (if filled) or b */
  uint8_t *hb = (hmtp.hdr_have >= 12U) ? hmtp.hdr_buf : b;

  if ( (hmtp.rx_state == RX_WAIT_OBJ_HDR || hmtp.rx_state == RX_WAIT_SI_DATA) || (xfer >= 12U) )
  {
    uint32_t len = rd32(hb+0);
    uint16_t type = rd16(hb+4);
    uint16_t code = rd16(hb+6);
    uint32_t tid  = rd32(hb+8);

    /* === Command containers === */
    if (type == PTP_CONTAINER_COMMAND)
    {
      hmtp.hdr_have = 0; /* reset */
      switch (code)
      {
        case PTP_OC_GetDeviceInfo:
        {
          uint32_t n = 0U;
          if (MTP_fops && MTP_fops->GetDeviceInfo) n = MTP_fops->GetDeviceInfo(hmtp.tx_buf+12, sizeof(hmtp.tx_buf)-12U);
          if (n == 0U) { MTP_SendResponse(pdev, PTP_RC_GENERAL_ERROR, tid); break; }
          MTP_SendDataAndResponse(pdev, PTP_OC_GetDeviceInfo, tid, hmtp.tx_buf+12, n);
        } break;

        case PTP_OC_OpenSession:
          hmtp.session_open = 1U;
          if (MTP_fops && MTP_fops->OpenSession) (void)MTP_fops->OpenSession(rd32(b+12));
          MTP_SendResponse(pdev, PTP_RC_OK, tid);
          break;

        case PTP_OC_CloseSession:
          hmtp.session_open = 0U;
          if (MTP_fops && MTP_fops->CloseSession) (void)MTP_fops->CloseSession();
          MTP_SendResponse(pdev, PTP_RC_OK, tid);
          break;

        case PTP_OC_GetStorageIDs:
        {
          uint32_t n = (MTP_fops && MTP_fops->GetStorageIDs) ? MTP_fops->GetStorageIDs(hmtp.tx_buf+12, sizeof(hmtp.tx_buf)-12U) : 0U;
          if (n == 0U) { MTP_SendResponse(pdev, PTP_RC_GENERAL_ERROR, tid); break; }
          MTP_SendDataAndResponse(pdev, PTP_OC_GetStorageIDs, tid, hmtp.tx_buf+12, n);
        } break;

        case PTP_OC_GetStorageInfo:
        {
          uint32_t sid = rd32(b+12);
          uint32_t n = (MTP_fops && MTP_fops->GetStorageInfo) ? MTP_fops->GetStorageInfo(hmtp.tx_buf+12, sizeof(hmtp.tx_buf)-12U, sid) : 0U;
          if (n == 0U) { MTP_SendResponse(pdev, PTP_RC_GENERAL_ERROR, tid); break; }
          MTP_SendDataAndResponse(pdev, PTP_OC_GetStorageInfo, tid, hmtp.tx_buf+12, n);
        } break;

        case PTP_OC_GetNumObjects:
        {
          uint32_t sid    = rd32(b+12);
          uint16_t format = (uint16_t)rd32(b+16);
          uint32_t parent = rd32(b+20);
          uint32_t count  = (MTP_fops && MTP_fops->CountObjects) ? MTP_fops->CountObjects(sid, format, parent) : 0U;
          wr32(hmtp.tx_buf+12, count);
          MTP_SendDataAndResponse(pdev, PTP_OC_GetNumObjects, tid, hmtp.tx_buf+12, 4U);
        } break;

        case PTP_OC_GetObjectHandles:
        {
          uint32_t sid    = rd32(b+12);
          uint16_t format = (uint16_t)rd32(b+16);
          uint32_t parent = rd32(b+20);
          uint32_t n = (MTP_fops && MTP_fops->GetObjectHandles) ?
                        MTP_fops->GetObjectHandles(hmtp.tx_buf+12, sizeof(hmtp.tx_buf)-12U, sid, format, parent) : 0U;
          if (n == 0U) { MTP_SendResponse(pdev, PTP_RC_GENERAL_ERROR, tid); break; }
          MTP_SendDataAndResponse(pdev, PTP_OC_GetObjectHandles, tid, hmtp.tx_buf+12, n);
        } break;

        case PTP_OC_GetObjectInfo:
        {
          uint32_t handle = rd32(b+12);
          uint32_t n = (MTP_fops && MTP_fops->GetObjectInfo) ? MTP_fops->GetObjectInfo(hmtp.tx_buf+12, sizeof(hmtp.tx_buf)-12U, handle) : 0U;
          if (n == 0U) { MTP_SendResponse(pdev, PTP_RC_GENERAL_ERROR, tid); break; }
          MTP_SendDataAndResponse(pdev, PTP_OC_GetObjectInfo, tid, hmtp.tx_buf+12, n);
        } break;

        case PTP_OC_GetObject:
        {
          uint32_t handle = rd32(b+12);
          uint32_t size = 0U;
          if (MTP_fops && MTP_fops->ObjectOpenRead && MTP_fops->ObjectReadChunk && MTP_fops->ObjectCloseRead)
          {
            if (MTP_fops->ObjectOpenRead(handle, &size) == 0U){
              if (size == 0U){ /* empty file */
                MTP_SendResponse(pdev, PTP_RC_OK, tid);
              } else {
                MTP_StreamStart(pdev, tid, size);
              }
              break;
            }
          }
          MTP_SendResponse(pdev, PTP_RC_GENERAL_ERROR, tid);
        } break;

        case PTP_OC_DeleteObject:
        {
          uint32_t handle = rd32(b+12);
          if (MTP_fops && MTP_fops->DeleteObject){
            if (MTP_fops->DeleteObject(handle)==0){
              MTP_SendResponse(pdev, PTP_RC_OK, tid);
            } else {
              MTP_SendResponse(pdev, PTP_RC_GENERAL_ERROR, tid);
            }
          } else {
            MTP_SendResponse(pdev, PTP_RC_OPERATION_NOT_SUPPORTED, tid);
          }
        } break;

        case PTP_OC_SendObjectInfo:
          hmtp.rx_state = RX_WAIT_SI_DATA;
          hmtp.rx_tid   = tid;
          hmtp.last_op  = code;
          hmtp.si_need  = 0;
          hmtp.si_got   = 0;
          hmtp.hdr_have = 0;
          break;

        case PTP_OC_SendObject:
          hmtp.rx_state = RX_WAIT_OBJ_HDR; /* waiting for header-containing first packet */
          hmtp.rx_tid   = tid;
          hmtp.last_op  = code;
          hmtp.pay_rcvd = 0;
          hmtp.write_offs = 0;
          hmtp.hdr_have = 0;
          break;

        default:
          MTP_SendResponse(pdev, PTP_RC_OPERATION_NOT_SUPPORTED, tid);
          break;
      }

      goto rearm;
    }

    /* === Data containers === */
    if (type == PTP_CONTAINER_DATA)
    {
      /* If header was accumulated separately, adjust b/xfer to point to payload following header */
      if (hmtp.hdr_have >= 12U){
        /* payload length may be smaller than our receive chunk */
        if (xfer > 0){
          /* already shifted earlier */
        }
      }

      if (code == PTP_OC_SendObjectInfo)
      {
        /* First packet may have header+payload; subsequent packets are pure payload */
        if (hmtp.rx_state == RX_WAIT_SI_DATA || hmtp.rx_state == RX_RECV_SI_MULTI){
          if (hmtp.rx_state == RX_WAIT_SI_DATA){
            hmtp.data_total = len;
            hmtp.pay_total  = (len>=12U)?(len-12U):0U;
            hmtp.si_need    = hmtp.pay_total;
            hmtp.si_got     = 0;
            hmtp.rx_state   = RX_RECV_SI_MULTI;
          }
          /* copy payload chunk from rx_buf (if we consumed header separately, xfer is payload) */
          uint8_t *payload = (hmtp.hdr_have>=12U) ? b : (b+12);
          uint32_t chunk   = (hmtp.hdr_have>=12U) ? xfer : ( (xfer>12U)?(xfer-12U):0U );
          if (chunk > hmtp.si_need - hmtp.si_got) chunk = hmtp.si_need - hmtp.si_got;
          if (chunk > sizeof(hmtp.si_buf) - hmtp.si_got) chunk = sizeof(hmtp.si_buf) - hmtp.si_got;
          if (chunk){
            memcpy(hmtp.si_buf + hmtp.si_got, payload, chunk);
            hmtp.si_got += chunk;
          }
          if (hmtp.si_got < hmtp.si_need){
            hmtp.hdr_have = 0;
            goto rearm; /* wait for more payload */
          }

          /* complete dataset now in si_buf — let if.c parse it via callback? We parse here minimal fields */
          /* We do not parse here; we pass dataset to if.c via getters? For simplicity parse here as before. */
          uint8_t *p = hmtp.si_buf;
          uint32_t avail = hmtp.si_got;
          if (avail < 4+2+2+4+2+4+4+4+4+4+4+4+2+4+4){ /* sanity */
            MTP_SendResponse(pdev, PTP_RC_INVALID_PARAMETER, hmtp.rx_tid);
            hmtp.rx_state = RX_IDLE; hmtp.hdr_have = 0;
            goto rearm;
          }
          uint32_t storage_id = (uint32_t)(p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24)); p+=4; avail-=4;
          uint16_t format     = (uint16_t)(p[0] | (p[1]<<8)); p+=2; avail-=2;
          p+=2; avail-=2; /* ProtectionStatus */
          uint32_t comp_size  = (uint32_t)(p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24)); p+=4; avail-=4;
          p += (2+4+4+4+4+4+4); avail -= (2+4+4+4+4+4+4);
          uint32_t parent     = (uint32_t)(p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24)); p+=4; avail-=4;
          p += 2; avail -= 2; /* AssociationType */
          p += 4; avail -= 4; /* AssociationDesc */
          p += 4; avail -= 4; /* SequenceNumber */

          /* Filename: PTP string — forward raw to if.c via ASCII sanitize there; here we keep as UTF-16 and expect if.c to handle name */
          /* For simplicity we attempt minimal ASCII decode here to extract a C string; if.c will sanitize again. */
          /* Simple decode: count at p[0], then 2B per char. */
          char name_ascii[128]; name_ascii[0]=0;
          if (avail > 0){
            uint8_t n = p[0];
            p++;
            for (uint32_t i=0;i<n && i< (sizeof(name_ascii)-1) && (i*2+1) < avail; i++){
              name_ascii[i] = (char)p[i*2]; /* low byte */
            }
            if (n < (avail/2)) { /* safe */ }
            name_ascii[(n < sizeof(name_ascii)-1)? n : (sizeof(name_ascii)-1)] = 0;
            p += n*2;
          }

          /* Remaining strings ignored (capture/mod/keywords) */

          /* Begin write via if.c */
          uint32_t new_handle = 0;
          if (MTP_fops && MTP_fops->ObjectBeginWrite){
            /* comp_size may be 0xFFFFFFFF meaning unknown */
            uint32_t size_for_check = (comp_size == 0xFFFFFFFFu) ? 0u : comp_size;
            if (MTP_fops->ObjectBeginWrite(storage_id, parent, name_ascii, size_for_check, format, &new_handle)==0){
              uint32_t params[3] = { storage_id, parent, new_handle };
              MTP_SendResponseParams(pdev, PTP_RC_OK, hmtp.rx_tid, params, 3);
            } else {
              MTP_SendResponse(pdev, PTP_RC_GENERAL_ERROR, hmtp.rx_tid);
            }
          } else {
            MTP_SendResponse(pdev, PTP_RC_OPERATION_NOT_SUPPORTED, hmtp.rx_tid);
          }

          hmtp.rx_state = RX_IDLE;
          hmtp.hdr_have = 0;
          goto rearm;
        }
      }

      if (code == PTP_OC_SendObject && (hmtp.rx_state == RX_WAIT_OBJ_HDR))
      {
        /* First DATA packet for SendObject: header + first payload piece */
        hmtp.data_total = len;
        hmtp.pay_total  = (len>=12U)?(len-12U):0U;
        hmtp.pay_rcvd   = 0;

        uint8_t *payload = (hmtp.hdr_have>=12U) ? b : (b+12);
        uint32_t chunk   = (hmtp.hdr_have>=12U) ? xfer : ( (xfer>12U)?(xfer-12U):0U );
        if (chunk > hmtp.pay_total) chunk = hmtp.pay_total;

        if (MTP_fops && MTP_fops->ObjectWriteChunk){
          uint32_t want = chunk;
          if (MTP_fops->ObjectWriteChunk(payload, hmtp.write_offs, &want) != 0){
            if (MTP_fops->ObjectEndWrite) MTP_fops->ObjectEndWrite(0);
            hmtp.rx_state = RX_IDLE;
            MTP_SendResponse(pdev, PTP_RC_GENERAL_ERROR, hmtp.rx_tid);
            hmtp.hdr_have = 0;
            goto rearm;
          }
          hmtp.write_offs += want;
          hmtp.pay_rcvd   += want;
        }

        if (hmtp.pay_rcvd >= hmtp.pay_total){
          if (MTP_fops && MTP_fops->ObjectEndWrite) MTP_fops->ObjectEndWrite(1);
          hmtp.rx_state = RX_IDLE;
          MTP_SendResponse(pdev, PTP_RC_OK, hmtp.rx_tid);
        } else {
          hmtp.rx_state = RX_RECV_OBJECT; /* continue receiving pure payload packets */
        }

        hmtp.hdr_have = 0;
        goto rearm;
      }

      /* Unhandled DATA */
      hmtp.hdr_have = 0;
      goto rearm;
    }
  }

rearm:
  USBD_LL_PrepareReceive(pdev, MTP_OUT_EP, hmtp.rx_buf, sizeof(hmtp.rx_buf));
  return (uint8_t)USBD_OK;
}

static uint8_t USBD_MTP_SOF(USBD_HandleTypeDef *pdev)
{ (void)pdev; return (uint8_t)USBD_OK; }

static uint8_t USBD_MTP_IsoINIncomplete(USBD_HandleTypeDef *pdev, uint8_t epnum)
{ (void)pdev; (void)epnum; return (uint8_t)USBD_OK; }

static uint8_t USBD_MTP_IsoOUTIncomplete(USBD_HandleTypeDef *pdev, uint8_t epnum)
{ (void)pdev; (void)epnum; return (uint8_t)USBD_OK; }

static uint8_t* USBD_MTP_GetFSConfigDescriptor(uint16_t *length)
{ *length = (uint16_t)sizeof(USBD_MTP_FS_CfgDesc); return USBD_MTP_FS_CfgDesc; }

static uint8_t* USBD_MTP_GetHSConfigDescriptor(uint16_t *length)
{ return USBD_MTP_GetFSConfigDescriptor(length); }

static uint8_t* USBD_MTP_GetOtherSpeedCfgDesc(uint16_t *length)
{ return USBD_MTP_GetFSConfigDescriptor(length); }

static uint8_t* USBD_MTP_GetDeviceQualifierDescriptor(uint16_t *length)
{ *length = (uint16_t)sizeof(USBD_MTP_DeviceQualifierDesc); return USBD_MTP_DeviceQualifierDesc; }
