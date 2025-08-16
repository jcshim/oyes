/* --------------------------------------------------------------------------
 * usbd_mtp_if.c — SafeSD (re-patched by ChatGPT, 2025-08-16)
 *  - Fix GetObjectHandles to respect (storage, format, parent) filters.
 *  - Add CountObjects for GetNumObjects.
 *  - Treat size==0xFFFFFFFF as unknown (no strict free-space check).
 *  - StorageType corrected to 0x0004 (Removable RAM).
 *  - Read path unchanged & robust.
 * -------------------------------------------------------------------------- */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#ifdef __GNUC__
#include <strings.h> /* strcasecmp */
#endif

#include "usbd_core.h"   /* USBD_HandleTypeDef */
#include "fatfs.h"       /* FatFs: f_opendir, f_readdir, f_open, f_stat, f_getfree, f_unlink */

/* ---- LE writers ---- */
static inline void wr16(uint8_t *p, uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static inline void wr32(uint8_t *p, uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static inline void wr64(uint8_t *p, uint64_t v){ for (int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }

/* ---- PTP String (ASCII -> UTF-16LE) ---- */
static uint32_t wr_ptp_string(uint8_t *dst, const char *s){
  if (!s || !*s){ dst[0]=0; return 1; }
  size_t n = strlen(s); if (n>255) n=255;
  dst[0]=(uint8_t)n;
  for (size_t i=0;i<n;i++){ dst[1+2*i]=(uint8_t)s[i]; dst[2+2*i]=0; }
  return (uint32_t)(1 + 2*n);
}

/* ----- 인터페이스 타입 (usbd_mtp.c와 동일해야 함) ----- */
typedef struct
{
  uint32_t (*GetDeviceInfo)(uint8_t *buf, uint32_t max);
  int      (*OpenSession)(uint32_t id);
  int      (*CloseSession)(void);
  uint32_t (*GetStorageIDs)(uint8_t *buf, uint32_t max);
  uint32_t (*GetStorageInfo)(uint8_t *buf, uint32_t max, uint32_t storage_id);

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

/* usbd_mtp.c 에서 제공 */
extern uint8_t USBD_MTP_RegisterInterface(USBD_HandleTypeDef *pdev, USBD_MTP_ItfTypeDef *fops);

/* ======= 구성 파라미터 ======= */
#define MTP_MAX_OBJECTS   (4096)     /* 폴더+파일 총 개수 상한 (확대) */
#define MTP_NAME_MAX      (128)
#define MTP_PATH_MAX      (256)

/* 단일 스토리지 ID */
static const uint32_t kStorageId = 0x00010001;

/* ======= Object Table ======= */
typedef struct {
  uint32_t handle;         /* 1..N (0이면 삭제/비어있음) */
  uint32_t parent;         /* 0 = storage root */
  uint32_t size;           /* 파일 크기, 폴더=0 */
  uint16_t format;         /* 파일 포맷, 폴더=0x3001 */
  uint16_t assoc_type;     /* 폴더=0x0001, 파일=0 */
  uint8_t  is_dir;         /* 1=dir, 0=file */
  char     name[MTP_NAME_MAX];
  char     path[MTP_PATH_MAX]; /* FatFs 절대 경로 */
} MtpObject;

static MtpObject s_objs[MTP_MAX_OBJECTS];
static uint32_t  s_count = 0;
static FIL       s_fil;         /* 읽기용 파일 핸들 */

/* 쓰기 상태 */
static struct {
  uint8_t  active;
  FIL      fil;
  char     path[MTP_PATH_MAX];
  uint32_t handle;
  uint32_t parent;
  uint32_t expected;
  uint32_t written;
  uint16_t format;
} s_w = {0};

/* ---- 유틸 ---- */
static void sanitize_name(const char *in, char *out, size_t outsz){
  if (!in || !out || outsz==0){ return; }
  size_t j=0;
  for (size_t i=0; in[i] && j+1<outsz; i++){
    unsigned char c = (unsigned char)in[i];
    if (c < 0x20 || c==0x7F || c=='\"' || c=='*' || c=='/' || c==':' || c=='<' ||
        c=='>' || c=='?' || c=='\\' || c=='|' ){
      out[j++] = '_';
    } else {
      out[j++] = (char)c;
    }
  }
  while (j>0 && (out[j-1]==' ' || out[j-1]=='.')) j--;
  if (j==0){
    const char *d = "noname";
    for (j=0; d[j] && j+1<outsz; j++) out[j]=d[j];
  }
  out[j] = '\0';
}

#ifndef FF_MAX_LFN
#define FF_MAX_LFN 255
#endif
#if FF_USE_LFN
static char s_lfn[FF_MAX_LFN+1];
#endif

static uint16_t ext_to_format(const char *name){
  const char *dot = strrchr(name, '.');
  if (!dot) return 0x3000; /* Undefined */
  if (!strcasecmp(dot, ".txt"))  return 0x3004; /* Text */
  if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return 0x3801;
  if (!strcasecmp(dot, ".png"))  return 0x380B;
  if (!strcasecmp(dot, ".mp4"))  return 0xB982; /* MP4 (MS) */
  return 0x3000;
}

static const MtpObject* find_by_handle(uint32_t h){
  if (h==0 || h> s_count) return NULL;
  const MtpObject *o = &s_objs[h-1];
  if (o->handle == h) return o;
  for (uint32_t i=0;i<s_count;i++) if (s_objs[i].handle==h) return &s_objs[i];
  return NULL;
}

static MtpObject* find_by_handle_mut(uint32_t h){
  if (h==0 || h> s_count) return NULL;
  MtpObject *o = &s_objs[h-1];
  if (o->handle == h) return o;
  for (uint32_t i=0;i<s_count;i++) if (s_objs[i].handle==h) return &s_objs[i];
  return NULL;
}

static int add_object(const char *path, const char *name,
                      uint32_t parent, uint8_t is_dir, uint32_t size){
  for (uint32_t i=0;i<s_count;i++){
    if (s_objs[i].handle==0){
      MtpObject *o=&s_objs[i];
      o->handle = i+1;
      o->parent = parent;
      o->is_dir = is_dir ? 1 : 0;
      o->size   = is_dir ? 0 : size;
      o->format = is_dir ? 0x3001 /* Association */ : ext_to_format(name);
      o->assoc_type = is_dir ? 0x0001 : 0;
      strncpy(o->name, name, MTP_NAME_MAX-1); o->name[MTP_NAME_MAX-1]='\0';
      strncpy(o->path, path, MTP_PATH_MAX-1); o->path[MTP_PATH_MAX-1]='\0';
      return (int)o->handle;
    }
  }
  if (s_count >= MTP_MAX_OBJECTS) return -1;
  MtpObject *o = &s_objs[s_count];
  o->handle = s_count + 1;
  o->parent = parent;
  o->is_dir = is_dir ? 1 : 0;
  o->size   = is_dir ? 0 : size;
  o->format = is_dir ? 0x3001 : ext_to_format(name);
  o->assoc_type = is_dir ? 0x0001 : 0;
  strncpy(o->name, name, MTP_NAME_MAX-1); o->name[MTP_NAME_MAX-1]='\0';
  strncpy(o->path, path, MTP_PATH_MAX-1); o->path[MTP_PATH_MAX-1]='\0';
  s_count++;
  return (int)o->handle;
}

static void join_path(char *dst, size_t dstsz, const char *dir, const char *name){
  size_t dl = strlen(dir);
  if (dl==0 || (dl==1 && dir[0]=='/')) { snprintf(dst, dstsz, "/%s", name); return; }
  if (dl>0 && dir[dl-1]=='/') snprintf(dst, dstsz, "%s%s", dir, name);
  else                        snprintf(dst, dstsz, "%s/%s", dir, name);
}

static const char* dirpath_from_handle(uint32_t parent){
  static char root[]="/";
  if (parent==0xFFFFFFFFu) return root; /* treat as storage root */
  if (parent==0) return root;
  const MtpObject *p = find_by_handle(parent);
  if (!p) return root;
  if (p->is_dir) return p->path;
  const MtpObject *pp = find_by_handle(p->parent);
  if (pp && pp->is_dir) return pp->path;
  return root;
}

static void scan_dir(const char *dir, uint32_t parent){
  DIR d; FILINFO fno;
#if FF_USE_LFN
  fno.lfname = s_lfn; fno.lfsize = sizeof(s_lfn);
#endif
  if (f_opendir(&d, dir) != FR_OK) return;

  for (;;){
    FRESULT fr = f_readdir(&d, &fno);
    if (fr != FR_OK || !fno.fname[0]) break;

    const char *nm =
#if FF_USE_LFN
      (fno.lfname && fno.lfname[0]) ? fno.lfname : fno.fname;
#else
      fno.fname;
#endif
    if (nm[0]=='.') continue;

    char child[MTP_PATH_MAX];
    join_path(child, sizeof(child), dir, nm);

    if (fno.fattrib & AM_DIR){
      int h_dir = add_object(child, nm, parent, 1, 0);
      if (h_dir >= 0) scan_dir(child, (uint32_t)h_dir);
    } else {
      add_object(child, nm, parent, 0, (uint32_t)fno.fsize);
    }

    if (s_count >= MTP_MAX_OBJECTS) break;
  }
  f_closedir(&d);
}

static void build_index(void){
  memset(s_objs, 0, sizeof(s_objs));
  s_count = 0;
  scan_dir("/", 0);
}

/* ====== DeviceInfo (최소) ====== */
static uint32_t IF_GetDeviceInfo(uint8_t *buf, uint32_t max){
  uint8_t *p = buf;
  if (max < 128) return 0;

  wr16(p, 0x0100); p+=2;            /* StandardVersion (1.00) */
  wr32(p, 6); p+=4;                 /* VendorExtensionID (MTP=6) */
  wr16(p, 0x0064); p+=2;            /* VendorExtensionVersion */
  p += wr_ptp_string(p, "MTP");
  wr16(p, 0x0000); p+=2;            /* FunctionalMode */

  /* OperationsSupported (Array of UINT16) */
  wr32(p, 10); p+=4;
  wr16(p, 0x1001); p+=2; /* GetDeviceInfo */
  wr16(p, 0x1002); p+=2; /* OpenSession */
  wr16(p, 0x1004); p+=2; /* GetStorageIDs */
  wr16(p, 0x1005); p+=2; /* GetStorageInfo */
  wr16(p, 0x1007); p+=2; /* GetObjectHandles */
  wr16(p, 0x1008); p+=2; /* GetObjectInfo */
  wr16(p, 0x1009); p+=2; /* GetObject */
  wr16(p, 0x100B); p+=2; /* DeleteObject */
  wr16(p, 0x100C); p+=2; /* SendObjectInfo */
  wr16(p, 0x100D); p+=2; /* SendObject */

  wr32(p, 0); p+=4; /* EventsSupported */
  wr32(p, 0); p+=4; /* DevicePropertiesSupported */
  wr32(p, 0); p+=4; /* CaptureFormats */
  wr32(p, 0); p+=4; /* PlaybackFormats */

  p += wr_ptp_string(p, "SHIM");
  p += wr_ptp_string(p, "SafeSD");
  p += wr_ptp_string(p, "3.2");
  p += wr_ptp_string(p, "0001");

  return (uint32_t)(p - buf);
}

static int IF_OpenSession(uint32_t sessionId){ (void)sessionId; build_index(); return 0; }
static int IF_CloseSession(void){
  if (s_w.active){
    f_close(&s_w.fil);
    f_unlink(s_w.path);
    memset(&s_w, 0, sizeof(s_w));
  }
  return 0;
}

/* StorageIDs: 단일 스토리지 */
static uint32_t IF_GetStorageIDs(uint8_t *buf, uint32_t max){
  if (max < 8) return 0;
  wr32(buf, 1);           /* count */
  wr32(buf+4, kStorageId);
  return 8;
}

static uint32_t IF_GetStorageInfo(uint8_t *buf, uint32_t max, uint32_t storageId){
  if (storageId != kStorageId || max < 256) return 0;
  uint8_t *p = buf;

  wr16(p, 0x0004); p+=2;  /* StorageType: Removable RAM */
  wr16(p, 0x0002); p+=2;  /* FilesystemType: Generic Hierarchical */
  wr16(p, 0x0001); p+=2;  /* AccessCapability: Read-Write */

  uint64_t maxcap = 0, freebytes = 0;
  DWORD free_clust = 0;
  FATFS *fs = NULL;

  if (f_getfree("", &free_clust, &fs) == FR_OK && fs){
    uint64_t clust_bytes = (uint64_t)fs->csize * 512ULL;
#if defined(__GNUC__) || defined(__CC_ARM) || defined(__ICCARM__)
    if (fs->n_fatent > 2){
      uint64_t total_clust = (uint64_t)(fs->n_fatent - 2);
      maxcap   = clust_bytes * total_clust;
      freebytes= clust_bytes * (uint64_t)free_clust;
    } else
#endif
    {
      freebytes = clust_bytes * (uint64_t)free_clust;
      maxcap    = 0;
    }
  }

  if (maxcap == 0 && freebytes > 0) maxcap = freebytes;
  if (maxcap == 0)   maxcap = (uint64_t)64 * 1024 * 1024;
  if (freebytes == 0) freebytes = (uint64_t)32 * 1024 * 1024;

  wr64(p, maxcap);   p+=8;  /* MaxCapacity */
  wr64(p, freebytes);p+=8;  /* FreeSpaceInBytes */
  wr32(p, 0);        p+=4;  /* FreeSpaceInImages */
  p += wr_ptp_string(p, "SafeSD");
  p += wr_ptp_string(p, "SDCard");

  return (uint32_t)(p - buf);
}

static int fmt_matches(uint16_t want, uint16_t have){
  if (want==0x0000) return 1; /* Undefined => all */
  if (want==0x3001) return have==0x3001; /* folder */
  return have!=0x3001; /* any file formats */
}

static uint32_t IF_GetObjectHandles(uint8_t *buf, uint32_t max,
                                    uint32_t storageId, uint16_t format, uint32_t parent){
  (void)storageId; /* 단일 스토리지 */
  /* Count first */
  uint32_t count = 0;
  for (uint32_t i=0;i<s_count;i++){
    const MtpObject *o = &s_objs[i];
    if (o->handle==0) continue;
    if (!fmt_matches(format, o->format)) continue;
    if (parent==0xFFFFFFFFu){
      /* all in storage */
      count++;
    } else {
      if (o->parent == parent) count++;
    }
  }
  uint32_t need = 4 + 4*count;
  if (max < need) return 0;
  uint8_t *p = buf;
  wr32(p, count); p+=4;
  for (uint32_t i=0;i<s_count;i++){
    const MtpObject *o = &s_objs[i];
    if (o->handle==0) continue;
    if (!fmt_matches(format, o->format)) continue;
    if (parent==0xFFFFFFFFu){
      wr32(p, o->handle); p+=4;
    } else if (o->parent == parent){
      wr32(p, o->handle); p+=4;
    }
  }
  return (uint32_t)(p - buf);
}

static uint32_t IF_CountObjects(uint32_t storageId, uint16_t format, uint32_t parent){
  (void)storageId;
  uint32_t count = 0;
  for (uint32_t i=0;i<s_count;i++){
    const MtpObject *o = &s_objs[i];
    if (o->handle==0) continue;
    if (!fmt_matches(format, o->format)) continue;
    if (parent==0xFFFFFFFFu) count++;
    else if (o->parent == parent) count++;
  }
  return count;
}

static uint32_t IF_GetObjectInfo(uint8_t *buf, uint32_t max, uint32_t handle){
  if (max < 256) return 0;
  const MtpObject *o = find_by_handle(handle);
  if (!o) return 0;
  uint8_t *p = buf;

  wr32(p, kStorageId); p+=4;      /* StorageID */
  wr16(p, o->format); p+=2;       /* ObjectFormat */
  wr16(p, 0); p+=2;               /* ProtectionStatus */
  wr32(p, o->is_dir ? 0 : o->size); p+=4; /* size (폴더=0) */
  memset(p, 0, 2+4+4+4+4+4+4); p += (2+4+4+4+4+4+4);
  wr32(p, (o->parent==0) ? 0xFFFFFFFFu : o->parent); p+=4; /* ParentObject */
  wr16(p, o->assoc_type); p+=2;   /* AssociationType (폴더=0x0001) */
  wr32(p, 0); p+=4;               /* AssociationDesc */
  wr32(p, 0); p+=4;               /* SequenceNumber */
  p += wr_ptp_string(p, o->name); /* Filename */
  p += wr_ptp_string(p, "");      /* DateCreated */
  p += wr_ptp_string(p, "");      /* DateModified */
  p += wr_ptp_string(p, "");      /* Keywords */
  return (uint32_t)(p - buf);
}

static int IF_ObjectOpenRead(uint32_t handle, uint32_t *size_bytes){
  const MtpObject *o = find_by_handle(handle);
  if (!o || o->is_dir) return -1;
  if (f_open(&s_fil, o->path, FA_READ) != FR_OK) return -1;
  *size_bytes = o->size;
  return 0;
}

static int IF_ObjectReadChunk(uint8_t *dst, uint32_t offs, uint32_t *len_inout){
  UINT br = 0;
  if (f_lseek(&s_fil, offs) != FR_OK) return -1;
  if (f_read(&s_fil, dst, *len_inout, &br) != FR_OK) return -1;
  *len_inout = br;
  return 0;
}

static void IF_ObjectCloseRead(void){
  f_close(&s_fil);
}

/* ====== 쓰기/삭제 구현 ====== */

static int IF_ObjectBeginWrite(uint32_t storage_id, uint32_t parent, const char *name,
                               uint32_t size_bytes, uint16_t format, uint32_t *out_handle)
{
  (void)storage_id;
  int h = -1;

  if (s_w.active) return -1; /* 이미 진행 중 */

  /* size_bytes==0xFFFFFFFF(unknown) → 0 으로 간주하여 free 체크 완화 */
  if (size_bytes == 0xFFFFFFFFu) size_bytes = 0;

  /* 여유공간 체크 (정보가 있으면만) */
  if (size_bytes > 0){
    DWORD  free_clust = 0;
    FATFS *fs = NULL;
    if (f_getfree("", &free_clust, &fs) == FR_OK && fs){
      uint64_t clust_bytes = (uint64_t)fs->csize * 512ULL;
      uint64_t freebytes = clust_bytes * (uint64_t)free_clust;
      if ((uint64_t)size_bytes > freebytes){
        return -1; /* 저장소 부족 */
      }
    }
  }

  char clean[MTP_NAME_MAX];
  sanitize_name(name ? name : "noname", clean, sizeof(clean));

  const char *dir = dirpath_from_handle(parent);
  char path[MTP_PATH_MAX];
  join_path(path, sizeof(path), dir, clean);

  if (format == 0x3001){ /* 폴더 생성 */
    FRESULT fr = f_mkdir(path);
    if (fr != FR_OK){
      FILINFO st;
#if FF_USE_LFN
      st.lfname = NULL; st.lfsize = 0;
#endif
      fr = f_stat(path, &st);
      if (fr != FR_OK || !(st.fattrib & AM_DIR)) return -1;
    }
    h = add_object(path, clean, parent, 1 /*is_dir*/, 0);
    if (h < 0) return -1;
    if (out_handle) *out_handle = (uint32_t)h;
    return 0;
  }

  /* 파일 케이스 */
  {
    FILINFO st;
#if FF_USE_LFN
    st.lfname = NULL; st.lfsize = 0;
#endif
    if (f_stat(path, &st) == FR_OK){
      if (st.fattrib & AM_DIR) return -1;
#if defined(FF_USE_CHMOD) && FF_USE_CHMOD
      if (st.fattrib & AM_RDO){
        (void)f_chmod(path, 0, AM_RDO);
      }
#endif
    }
  }

  FRESULT fr = f_open(&s_w.fil, path, FA_WRITE | FA_CREATE_ALWAYS);
  if (fr != FR_OK) return -1;

  h = add_object(path, clean, parent, 0 /*file*/, 0);
  if (h < 0){
    f_close(&s_w.fil);
    f_unlink(path);
    return -1;
  }

  s_w.active   = 1;
  strncpy(s_w.path, path, sizeof(s_w.path)-1);
  s_w.path[sizeof(s_w.path)-1] = '\0';
  s_w.handle   = (uint32_t)h;
  s_w.parent   = parent;
  s_w.expected = size_bytes;   /* 0이면 미지 */
  s_w.written  = 0;
  s_w.format   = format;

  MtpObject *o = find_by_handle_mut((uint32_t)h);
  if (o){
    o->format = format ? format : ext_to_format(clean);
  }

  if (out_handle) *out_handle = (uint32_t)h;
  return 0;
}

static int IF_ObjectWriteChunk(const uint8_t *data, uint32_t offs, uint32_t *len_inout)
{
  if (!s_w.active) return -1;

  if (f_lseek(&s_w.fil, offs) != FR_OK) return -1;

  UINT bw = 0;
  if (f_write(&s_w.fil, data, *len_inout, &bw) != FR_OK) return -1;

  *len_inout = bw;
  s_w.written = (offs + bw > s_w.written) ? (offs + bw) : s_w.written;
  return 0;
}

static int IF_ObjectEndWrite(int commit)
{
  if (!s_w.active) return 0;

  f_sync(&s_w.fil);
  f_close(&s_w.fil);

  if (commit || s_w.written > 0){
    MtpObject *o = find_by_handle_mut(s_w.handle);
    if (o){
      o->size = s_w.written;
    }
  } else {
    f_unlink(s_w.path);
    MtpObject *o = find_by_handle_mut(s_w.handle);
    if (o){ o->handle = 0; }
  }

  memset(&s_w, 0, sizeof(s_w));
  return 0;
}

static int IF_DeleteObject(uint32_t handle)
{
  MtpObject *o = find_by_handle_mut(handle);
  if (!o) return -1;
  if (o->is_dir) return -1; /* 폴더 삭제 미지원 */
  if (f_unlink(o->path) != FR_OK) return -1;
  o->handle = 0;
  return 0;
}

/* ====== 인터페이스 바인딩 ====== */
USBD_MTP_ItfTypeDef USBD_MTP_fops = {
  .GetDeviceInfo    = IF_GetDeviceInfo,
  .OpenSession      = IF_OpenSession,
  .CloseSession     = IF_CloseSession,
  .GetStorageIDs    = IF_GetStorageIDs,
  .GetStorageInfo   = IF_GetStorageInfo,
  .GetObjectHandles = IF_GetObjectHandles,
  .CountObjects     = IF_CountObjects,
  .GetObjectInfo    = IF_GetObjectInfo,
  .ObjectOpenRead   = IF_ObjectOpenRead,
  .ObjectReadChunk  = IF_ObjectReadChunk,
  .ObjectCloseRead  = IF_ObjectCloseRead,

  .ObjectBeginWrite = IF_ObjectBeginWrite,
  .ObjectWriteChunk = IF_ObjectWriteChunk,
  .ObjectEndWrite   = IF_ObjectEndWrite,
  .DeleteObject     = IF_DeleteObject,
};

/* CubeMX 호환용 래퍼 */
uint8_t USBD_MTP_If_Register(USBD_HandleTypeDef *pdev)
{
  return USBD_MTP_RegisterInterface(pdev, &USBD_MTP_fops);
}
