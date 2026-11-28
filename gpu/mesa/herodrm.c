/*
 * libdrm stand-in for Mesa's Panfrost on the Galaxy S7's iOS: the panfrost
 * DRM ioctls Mesa issues, answered with the kernel's HeroGPU IOKit user
 * client (iokit/Kernel/arm/HeroGPU.cpp). Built as libdrm.2.dylib; Mesa
 * links it in place of the real libdrm (headers from libdrm 2.4.124).
 *
 * One GPU, one connection per process. The "fd" Mesa holds is a real file
 * descriptor on /dev/null (Mesa dups and closes it); every call is ours.
 * BO mmap goes through herodrm_mmap() (Mesa's pan_kmod_bo_mmap is patched
 * to call it on Apple): MMAP_BO hands out handle << 32 as the offset.
 * Submit is synchronous in the kernel, so syncobjs are always signaled.
 *
 * Environment: HERODRM_HEAP_MB caps the tiler heap BO (default 32; the
 * kernel buffers are wired, Mesa asks for 128 MiB), HERODRM_DEBUG=1 logs
 * every call to the kernel log.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include "drm-uapi/panfrost_drm.h"

#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>

/* IOKit, declared here: the iOS SDK ships IOKit without its headers. */
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef mach_port_t io_connect_t;
extern const mach_port_t kIOMainPortDefault;
CFMutableDictionaryRef IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t main, CFDictionaryRef matching);
kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type,
                            io_connect_t *connect);
kern_return_t IOObjectRelease(io_object_t object);
kern_return_t IOConnectCallMethod(io_connect_t connection, uint32_t selector,
                                  const uint64_t *input, uint32_t inputCnt,
                                  const void *inputStruct, size_t inputStructCnt,
                                  uint64_t *output, uint32_t *outputCnt,
                                  void *outputStruct, size_t *outputStructCnt);

#define EXPORT __attribute__((visibility("default")))

enum { SEL_PARAM, SEL_CREATE, SEL_FREE, SEL_MAP, SEL_SUBMIT, SEL_LOG, SEL_SURFACE_BO, SEL_SYNC_BO, SEL_SUBMIT_BATCH, SEL_ENQUEUE, SEL_WAIT_SEQ, SEL_ENQUEUE_BATCH };

#define MAX_BOS 4096

struct hbo {
   uint32_t khandle;
   uint64_t va, size;
   uint64_t cpu;
   bool in_use;
   bool evictable;
   bool purged;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static io_connect_t conn;
static struct hbo bos[MAX_BOS + 1];     /* indexed by userspace handle (khandle = kernel handle, 0 if purged) */
static uint64_t evictable_bytes;
#define MAX_PRIME 256
static struct { int fd; uint32_t handle; uint32_t surface_id; } primes[MAX_PRIME];  /* imported surfaces' stand-in descriptors */
static int debug = -1;
static uint64_t submits, faults;

/* Where the time goes, logged every 5 s while the GPU is used: submits (GPU
 * time as the kernel measured it, and the whole call) and BO creation (the
 * kernel zeroes, cleans and maps every new buffer). */
static struct {
   uint64_t start_ns, submits, gpu_us, submit_ns, creates, create_bytes, create_ns, frees;
   uint64_t sel_calls[12], sel_ns[12];  /* (2026-11-14) every kernel call, by selector: where mach_msg2_trap's time goes */
} st;
static const char *const sel_names[12] = { "param", "create", "free", "map", "submit", "log", "surfacebo", "syncbo", "batch", "enq", "wait", "enqb" };

EXPORT void herodrm_log(const char *fmt, ...);

static uint64_t
now_ns(void)
{
   return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static void
stats_tick(uint64_t t)
{
   if (!st.start_ns) {
      st.start_ns = t;
      return;
   }
   uint64_t wall = t - st.start_ns;
   if (wall < 5000000000ull)
      return;
   herodrm_log("stats %.1f s: %llu submits, GPU %.1f ms (%.0f%%), submit calls %.1f ms | "
               "%llu new BOs %.1f MB in %.1f ms | %llu freed",
               wall / 1e9, (unsigned long long)st.submits, st.gpu_us / 1e3,
               100.0 * st.gpu_us * 1e3 / wall, st.submit_ns / 1e6,
               (unsigned long long)st.creates, st.create_bytes / 1048576.0, st.create_ns / 1e6,
               (unsigned long long)st.frees);
   char by[512];
   int used = 0;
   for (int i = 0; i < 12 && used < (int)sizeof(by) - 48; i++) {
      if (st.sel_calls[i] && i != 5)
         used += snprintf(by + used, sizeof(by) - used, " %s %llu/%.1fms", sel_names[i],
                          (unsigned long long)st.sel_calls[i], st.sel_ns[i] / 1e6);
   }
   if (used)
      herodrm_log("kernel calls %.1f s:%s", wall / 1e9, by);
   memset(&st, 0, sizeof(st));
   st.start_ns = t;
}

static uint32_t
call(uint32_t sel, const uint64_t *in, uint32_t nin, uint64_t *out, uint32_t nout)
{
   uint32_t n = nout;
   uint64_t t0 = now_ns();
   kern_return_t kr = IOConnectCallMethod(conn, sel, in, nin, NULL, 0, out, &n, NULL, NULL);
   if (sel < 12) {
      st.sel_calls[sel]++;
      st.sel_ns[sel] += now_ns() - t0;
   }
   return kr;
}

static int connect_gpu(void);

EXPORT void
herodrm_log(const char *fmt, ...)
{
   char line[1024];
   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(line, sizeof(line), fmt, ap);
   va_end(ap);
   if (n < 0)
      return;
   if (n >= (int)sizeof(line))
      n = sizeof(line) - 1;
   if (!conn)
      connect_gpu();    /* a test's first lines; not under the lock */
   if (conn)
      IOConnectCallMethod(conn, SEL_LOG, NULL, 0, line, n, NULL, NULL, NULL, NULL);
   fprintf(stderr, "herodrm: %s\n", line);
}

#define DBG(...) do { if (debug > 0) herodrm_log(__VA_ARGS__); } while (0)

static int
fail(int e)
{
   errno = e;
   return -1;
}

/* Open the HeroGPU user client once; 0 on success. */
static int
connect_gpu(void)
{
   if (conn)
      return 0;
   if (debug < 0) {
      const char *d = getenv("HERODRM_DEBUG");
      debug = d && *d == '1';
   }
   io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                  IOServiceMatching("HeroGPU"));
   if (!svc)
      return -1;
   kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
   IOObjectRelease(svc);
   if (kr != KERN_SUCCESS) {
      conn = 0;
      return -1;
   }
   DBG("connected");
   return 0;
}

EXPORT int
drmOpenWithType(const char *name, const char *busid, int type)
{
   (void)busid;
   (void)type;
   if (name && strcmp(name, "panfrost") != 0)
      return fail(ENODEV);
   pthread_mutex_lock(&lock);
   int r = connect_gpu();
   pthread_mutex_unlock(&lock);
   if (r)
      return fail(ENODEV);
   return open("/dev/null", O_RDWR | O_CLOEXEC);
}

EXPORT int
drmOpen(const char *name, const char *busid)
{
   return drmOpenWithType(name, busid, 0);
}

EXPORT int
drmClose(int fd)
{
   return close(fd);
}

EXPORT drmVersionPtr
drmGetVersion(int fd)
{
   (void)fd;
   drmVersionPtr v = calloc(1, sizeof(*v));
   if (!v)
      return NULL;
   /* 1.2: HEAP/NOEXEC BOs and MADVISE; no timestamp query (1.3). */
   v->version_major = 1;
   v->version_minor = 2;
   v->name = strdup("panfrost");
   v->name_len = 8;
   v->date = strdup("20251025");
   v->date_len = 8;
   v->desc = strdup("HeroGPU Mali-T880 (Galaxy S7)");
   v->desc_len = (int)strlen(v->desc);
   return v;
}

EXPORT void
drmFreeVersion(drmVersionPtr v)
{
   if (!v)
      return;
   free(v->name);
   free(v->date);
   free(v->desc);
   free(v);
}

EXPORT int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
   (void)fd;
   (void)capability;
   *value = 0;
   return 0;
}

static unsigned
heap_cap(void)
{
   const char *s = getenv("HERODRM_HEAP_MB");
   unsigned mb = s ? (unsigned)atoi(s) : 32;
   return (mb ? mb : 32) << 20;
}

static bool async_on, kfifo_on, kfifo_batch_on;
static uint64_t kfifo_done, kfifo_gpu_us, kfifo_seq;
static uint64_t bo_last_job[MAX_BOS + 1];       /* by handle: the count of queued jobs when the last one naming it went on */
static void queue_drain(void);
static void kfifo_flush_batch(void);
static bool bo_busy(uint32_t handle);
static void bo_wait(uint32_t handle);

static uint32_t
alloc_uhandle(void)
{
   for (uint32_t h = 1; h <= MAX_BOS; h++)
      if (!bos[h].in_use)
         return h;
   return 0;
}

static bool
is_prime_handle(uint32_t h)
{
   for (int i = 0; i < MAX_PRIME; i++)
      if (primes[i].handle == h)
         return true;
   return false;
}

/* Reclaim kernel pages + GPU VA from Mesa's DONTNEED-cached BOs until evictable_bytes <= target_bytes. */
static unsigned
purge_evictable(uint64_t target_bytes, bool large_only)
{
   unsigned purged_n = 0;
   for (uint32_t h = 1; h <= MAX_BOS && evictable_bytes > target_bytes; h++) {
      struct hbo *bo = &bos[h];
      if (!bo->in_use || !bo->evictable || bo->purged || !bo->khandle)
         continue;
      if (large_only && bo->size < (1024u * 1024u))
         continue;
      if (bo_busy(h) || is_prime_handle(h))
         continue;
      uint64_t in = bo->khandle;
      call(SEL_FREE, &in, 1, NULL, 0);
      st.frees++;
      evictable_bytes -= bo->size;
      bo_last_job[h] = 0;
      bo->khandle = 0;
      bo->va = 0;
      bo->size = 0;
      bo->cpu = 0;
      bo->evictable = false;
      bo->purged = true;
      purged_n++;
   }
   return purged_n;
}

static int
create_bo(struct drm_panfrost_create_bo *req)
{
   uint64_t size = req->size;
   if (req->flags & PANFROST_BO_HEAP && size > heap_cap())
      size = heap_cap();
   uint32_t u = alloc_uhandle();
   if (!u) {
      herodrm_log("CREATE_BO: userspace BO handle table full");
      return fail(ENOMEM);
   }
   uint64_t in = size, out[3] = {0};
   uint64_t t0 = now_ns();
   uint32_t kr = call(SEL_CREATE, &in, 1, out, 3);
   if (kr || out[0] == 0 || out[0] > MAX_BOS) {
      unsigned p = purge_evictable(0, false);
      if (p == 0 && evictable_bytes > 0) {
         queue_drain();
         p = purge_evictable(0, false);
      }
      if (p > 0) {
         kr = call(SEL_CREATE, &in, 1, out, 3);
         if (kr == 0 && out[0] > 0 && out[0] <= MAX_BOS)
            herodrm_log("CREATE_BO %llu B recovered after purging %u cached BOs", (unsigned long long)size, p);
      }
   }
   uint64_t t1 = now_ns();
   st.creates++;
   st.create_bytes += size;
   st.create_ns += t1 - t0;
   stats_tick(t1);
   if (kr || out[0] == 0 || out[0] > MAX_BOS) {
      herodrm_log("CREATE_BO %llu bytes (flags 0x%x) failed: 0x%x",
                  (unsigned long long)size, req->flags, kr);
      return fail(ENOMEM);
   }
   req->handle = u;
   req->offset = out[1];
   /* Mesa takes the BO's size from here (pan_kmod_bo_init): a capped heap
    * must not be described to the tiler as the size asked for. */
   req->size = (uint32_t)out[2];
   bos[u] = (struct hbo){ .khandle = (uint32_t)out[0], .va = out[1], .size = out[2], .in_use = true };
   DBG("CREATE_BO %u (k%u): %llu bytes at 0x%llx flags 0x%x", req->handle, (unsigned)out[0],
       (unsigned long long)out[2], (unsigned long long)out[1], req->flags);
   return 0;
}

static struct hbo *
lookup(uint32_t handle)
{
   return handle && handle <= MAX_BOS && bos[handle].in_use && !bos[handle].purged && bos[handle].va
          ? &bos[handle] : NULL;
}

static int
free_bo(uint32_t handle)
{
   if (!handle || handle > MAX_BOS || !bos[handle].in_use)
      return fail(EINVAL);
   if (bos[handle].purged) {
      memset(&bos[handle], 0, sizeof(bos[handle]));
      return 0;
   }
   if (bos[handle].evictable) {
      evictable_bytes -= bos[handle].size;
      bos[handle].evictable = false;
   }
   for (int i = 0; i < MAX_PRIME; i++)
      if (primes[i].handle == handle) {      /* an imported surface: its stand-in descriptor goes too */
         close(primes[i].fd);
         primes[i].handle = 0;
         primes[i].surface_id = 0;
      }
   bo_wait(handle);
   bo_last_job[handle] = 0;
   uint64_t in = bos[handle].khandle;
   call(SEL_FREE, &in, 1, NULL, 0);
   st.frees++;
   memset(&bos[handle], 0, sizeof(bos[handle]));
   DBG("FREE_BO %u", handle);
   return 0;
}

/* CPU address of GPU VA va (mapping the BO if needed), or NULL. */
static const uint8_t *
gpu_to_cpu(uint64_t va, uint64_t len)
{
   for (unsigned h = 1; h <= MAX_BOS; h++) {
      struct hbo *bo = &bos[h];
      if (!bo->in_use || bo->purged || !bo->va || va < bo->va || va + len > bo->va + bo->size)
         continue;
      if (!bo->cpu) {
         uint64_t in = bo->khandle, out = 0;
         if (call(SEL_MAP, &in, 1, &out, 1) == 0)
            bo->cpu = out;
      }
      return bo->cpu ? (const uint8_t *)(uintptr_t)(bo->cpu + (va - bo->va)) : NULL;
   }
   return NULL;
}

static void
dump_words(const char *what, uint64_t va, unsigned words)
{
   const uint32_t *p = (const uint32_t *)gpu_to_cpu(va, words * 4);
   if (!p) {
      herodrm_log("  %s 0x%llx: not in any BO", what, (unsigned long long)va);
      return;
   }
   for (unsigned i = 0; i < words; i += 8)
      herodrm_log("  %s 0x%llx: %08x %08x %08x %08x %08x %08x %08x %08x", what,
                  (unsigned long long)(va + i * 4), p[i], p[i + 1], p[i + 2], p[i + 3],
                  p[i + 4], p[i + 5], p[i + 6], p[i + 7]);
}

/* After a fault: every job of the chain, and the payload and fault pointer
 * of the ones that did not finish. Called with the lock held. */
static void
dump_chain(uint64_t jc)
{
   static const char *types[] = { "?", "null", "write_value", "cache_flush", "compute",
                                  "vertex", "geometry", "tiler", "fused", "fragment" };
   for (unsigned n = 0; jc && n < 16; n++) {
      const uint8_t *h = gpu_to_cpu(jc, 32);
      if (!h) {
         herodrm_log("  job 0x%llx: not in any BO", (unsigned long long)jc);
         return;
      }
      uint32_t status = *(const uint32_t *)h;
      uint64_t fault = *(const uint64_t *)(h + 8), next = *(const uint64_t *)(h + 24);
      unsigned type = (h[16] >> 1) & 0x7f;
      herodrm_log("  job 0x%llx %s: status 0x%x first_incomplete %u fault_ptr 0x%llx "
                  "index %u deps %u,%u next 0x%llx",
                  (unsigned long long)jc, type < 10 ? types[type] : "?", status,
                  *(const uint32_t *)(h + 4), (unsigned long long)fault,
                  *(const uint16_t *)(h + 18), *(const uint16_t *)(h + 20),
                  *(const uint16_t *)(h + 22), (unsigned long long)next);
      if (status != 1) {        /* not DONE */
         dump_words("payload", jc + 32, 48);
         if (fault)
            dump_words("at fault_ptr", fault & ~31ull, 16);
      }
      jc = h[16] & 1 ? next : (uint32_t)*(const uint32_t *)(h + 24);
   }
}

static int
submit_now(uint64_t jc, uint64_t js)
{
   uint64_t in[2] = { jc, js }, out[2] = {0};
   uint64_t t0 = now_ns();
   uint32_t kr = call(SEL_SUBMIT, in, 2, out, 2);
   uint64_t t1 = now_ns();
   submits++;
   st.submits++;
   st.gpu_us += out[1];
   st.submit_ns += t1 - t0;
   stats_tick(t1);
   if (kr || out[0]) {
      faults++;
      if (faults <= 16 || debug > 0)
         herodrm_log("SUBMIT #%llu jc 0x%llx js %llu: kr 0x%x status 0x%llx (%llu us)",
                     (unsigned long long)submits, (unsigned long long)jc,
                     (unsigned long long)js, kr, (unsigned long long)out[0],
                     (unsigned long long)out[1]);
      if (faults <= 2 && !async_on)
         dump_chain(jc);        /* (the BO table is the caller's, under its lock) */
   } else {
      DBG("SUBMIT #%llu jc 0x%llx js %llu: %llu us", (unsigned long long)submits,
          (unsigned long long)jc, (unsigned long long)js, (unsigned long long)out[1]);
   }
   /* A faulting job is reported, not fatal: Mesa would abort on an error. */
   return kr ? fail(EIO) : 0;
}

/*
 * HERO_ASYNC=1: a submit goes on a queue and returns; a thread of its own
 * makes the kernel's call, which lasts as long as the job. In order, so one
 * job's results are there for the next, as the kernel's synchronous submits
 * always had it. A wait on a BO, or freeing one, waits for the last job that
 * named it (a submit lists its BOs); a wait on a syncobj or cleaning a
 * surface's caches drains the queue. The worker takes no lock of the
 * callers', so they may wait with theirs held.
 */
#define QUEUE_SIZE 64
static struct { uint64_t jc, js; } queue[QUEUE_SIZE];
static uint64_t queued, finished;       /* counts; queue[n % QUEUE_SIZE] */
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qwork = PTHREAD_COND_INITIALIZER, qdone = PTHREAD_COND_INITIALIZER;

static bool batch_supported = true;

static void
submit_batch_now(const uint64_t *pairs, unsigned count)
{
   if (count == 1 || !batch_supported) {
      for (unsigned i = 0; i < count; i++)
         submit_now(pairs[2 * i], pairs[2 * i + 1]);
      return;
   }
   uint64_t out[2] = {0};
   uint32_t nout = 2;
   uint64_t t0 = now_ns();
   kern_return_t kr = IOConnectCallMethod(conn, SEL_SUBMIT_BATCH, NULL, 0,
                                          pairs, (size_t)count * 16, out, &nout, NULL, NULL);
   uint64_t t1 = now_ns();
   if (kr == (kern_return_t)0xe00002c7) { /* kIOReturnUnsupported: older kernel */
      batch_supported = false;
      for (unsigned i = 0; i < count; i++)
         submit_now(pairs[2 * i], pairs[2 * i + 1]);
      return;
   }
   st.sel_calls[SEL_SUBMIT_BATCH]++;
   st.sel_ns[SEL_SUBMIT_BATCH] += t1 - t0;
   submits += count;
   st.submits += count;
   st.gpu_us += out[1];
   st.submit_ns += t1 - t0;
   stats_tick(t1);
   if (kr || out[0]) {
      faults++;
      if (faults <= 16 || debug > 0)
         herodrm_log("BATCH #%llu (%u jobs): kr 0x%x status 0x%llx (%llu us)",
                     (unsigned long long)submits, count, kr, (unsigned long long)out[0],
                     (unsigned long long)out[1]);
   }
}

static void *
queue_worker(void *arg)
{
   (void)arg;
   pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
   pthread_mutex_lock(&qlock);
   for (;;) {
      while (finished == queued)
         pthread_cond_wait(&qwork, &qlock);
      uint64_t pairs[QUEUE_SIZE * 2];
      unsigned n = 0;
      while (finished + n < queued && n < QUEUE_SIZE) {
         pairs[2 * n] = queue[(finished + n) % QUEUE_SIZE].jc;
         pairs[2 * n + 1] = queue[(finished + n) % QUEUE_SIZE].js;
         n++;
      }
      pthread_mutex_unlock(&qlock);
      submit_batch_now(pairs, n);
      pthread_mutex_lock(&qlock);
      finished += n;
      pthread_cond_broadcast(&qdone);
   }
   return NULL;
}

#define ENQ_BATCH_MAX 32
#define ENQ_BO_PENDING (~0ull)
static uint64_t enq_pairs[ENQ_BATCH_MAX * 2];
static unsigned enq_n;
static uint32_t enq_bos[512];
static unsigned enq_nbos;

static void
kfifo_flush_batch(void)
{
   if (!kfifo_batch_on || enq_n == 0 || !conn)
      return;
   __asm__ volatile ("dsb sy" ::: "memory");
   uint64_t out[3] = {0};
   uint32_t nout = 3;
   uint64_t t0 = now_ns();
   kern_return_t kr = IOConnectCallMethod(conn, SEL_ENQUEUE_BATCH, NULL, 0,
                                          enq_pairs, enq_n * 2 * sizeof(uint64_t),
                                          out, &nout, NULL, NULL);
   uint64_t t1 = now_ns();
   st.sel_calls[SEL_ENQUEUE_BATCH]++;
   st.sel_ns[SEL_ENQUEUE_BATCH] += t1 - t0;
   st.submit_ns += t1 - t0;
   enq_n = 0;
   uint64_t real_seq = (kr == 0 && out[0] != 0) ? out[0] : kfifo_seq;
   if (kr == 0 && out[0] != 0) {
      kfifo_seq = out[0];
      __atomic_store_n(&kfifo_done, out[1], __ATOMIC_RELEASE);
      if (out[2] >= kfifo_gpu_us) {
         st.gpu_us += out[2] - kfifo_gpu_us;
         kfifo_gpu_us = out[2];
      }
   }
   for (unsigned i = 0; i < enq_nbos; i++) {
      uint32_t h = enq_bos[i];
      if (h && h <= MAX_BOS && bo_last_job[h] == ENQ_BO_PENDING)
         bo_last_job[h] = real_seq;
   }
   enq_nbos = 0;
   stats_tick(t1);
}

EXPORT void
herodrm_flush(void)
{
   pthread_mutex_lock(&lock);
   kfifo_flush_batch();
   pthread_mutex_unlock(&lock);
}

static void
kfifo_wait(uint64_t seq, bool nonblock)
{
   kfifo_flush_batch();
   uint64_t in[2] = { seq, nonblock ? 1 : 0 }, out[2] = {0};
   if (conn && call(SEL_WAIT_SEQ, in, 2, out, 2) == 0) {
      __atomic_store_n(&kfifo_done, out[0], __ATOMIC_RELEASE);
      if (out[1] >= kfifo_gpu_us) {
         st.gpu_us += out[1] - kfifo_gpu_us;
         kfifo_gpu_us = out[1];
      }
   }
}

static void
queue_drain(void)
{
   if (kfifo_on) {
      kfifo_wait(0, false);
      return;
   }
   if (!async_on)
      return;
   pthread_mutex_lock(&qlock);
   while (finished != queued)
      pthread_cond_wait(&qdone, &qlock);
   pthread_mutex_unlock(&qlock);
}

/* Is the last job that named this BO still queued or running? */
static bool
bo_busy(uint32_t handle)
{
   if (!handle || handle > MAX_BOS)
      return false;
   if (kfifo_on) {
      uint64_t need = bo_last_job[handle];
      if (need == 0)
         return false;
      if (need == ENQ_BO_PENDING) {
         kfifo_flush_batch();
         need = bo_last_job[handle];
      }
      if (need <= __atomic_load_n(&kfifo_done, __ATOMIC_ACQUIRE))
         return false;
      kfifo_wait(0, true);
      return need > __atomic_load_n(&kfifo_done, __ATOMIC_ACQUIRE);
   }
   if (!async_on)
      return false;
   pthread_mutex_lock(&qlock);
   bool busy = finished < bo_last_job[handle];
   pthread_mutex_unlock(&qlock);
   return busy;
}

static void
bo_wait(uint32_t handle)
{
   if (!handle || handle > MAX_BOS)
      return;
   if (kfifo_on) {
      uint64_t need = bo_last_job[handle];
      if (need == ENQ_BO_PENDING) {
         kfifo_flush_batch();
         need = bo_last_job[handle];
      }
      if (need > __atomic_load_n(&kfifo_done, __ATOMIC_ACQUIRE))
         kfifo_wait(need == ENQ_BO_PENDING ? 0 : need, false);
      return;
   }
   if (!async_on)
      return;
   pthread_mutex_lock(&qlock);
   while (finished < bo_last_job[handle])
      pthread_cond_wait(&qdone, &qlock);
   pthread_mutex_unlock(&qlock);
}

static int
submit(struct drm_panfrost_submit *s)
{
   static bool asked;
   uint64_t js = (s->requirements & PANFROST_JD_REQ_FS) ? 0 : 1;

   if (!asked) {
      const char *a = getenv("HERO_ASYNC");
      pthread_t thread;

      asked = true;
      bool want_async = !(a && *a == '0');
      if (want_async) {
         uint64_t in[2] = { 0, 1 }, out[3] = {0};
         if (call(SEL_WAIT_SEQ, in, 2, out, 2) == 0) {
            kfifo_on = true;
            __atomic_store_n(&kfifo_done, out[0], __ATOMIC_RELEASE);
            kfifo_gpu_us = out[1];
            uint32_t nout = 3;
            if (IOConnectCallMethod(conn, SEL_ENQUEUE_BATCH, NULL, 0, NULL, 0, out, &nout, NULL, NULL) == 0) {
               kfifo_batch_on = true;
               kfifo_seq = out[0];
               __atomic_store_n(&kfifo_done, out[1], __ATOMIC_RELEASE);
               kfifo_gpu_us = out[2];
            }
         }
      }
      if (!kfifo_on)
         async_on = want_async && pthread_create(&thread, NULL, queue_worker, NULL) == 0;
      herodrm_log("submits are %s",
                  kfifo_batch_on ? "kernel FIFO batched (0376)" :
                  kfifo_on ? "kernel FIFO (0357)" :
                  async_on ? "queued + batched (HERO_ASYNC=1)" : "synchronous (HERO_ASYNC=0)");
   }
   if (kfifo_batch_on) {
      if (s->jc == 0)
         return 0;
      enq_pairs[2 * enq_n] = s->jc;
      enq_pairs[2 * enq_n + 1] = js;
      enq_n++;
      submits++;
      st.submits++;
      const uint32_t *handles = (const uint32_t *)(uintptr_t)s->bo_handles;
      for (uint32_t i = 0; i < s->bo_handle_count; i++) {
         uint32_t h = handles[i];
         if (h && h <= MAX_BOS) {
            if (bo_last_job[h] != ENQ_BO_PENDING) {
               if (enq_nbos < 512)
                  enq_bos[enq_nbos++] = h;
               bo_last_job[h] = ENQ_BO_PENDING;
            }
         }
      }
      if (enq_n >= ENQ_BATCH_MAX || enq_nbos >= 480)
         kfifo_flush_batch();
      return 0;
   }
   if (kfifo_on) {
      __asm__ volatile ("dsb sy" ::: "memory");
      uint64_t in[2] = { s->jc, js }, out[3] = {0};
      uint64_t t0 = now_ns();
      uint32_t kr = call(SEL_ENQUEUE, in, 2, out, 3);
      uint64_t t1 = now_ns();
      if (kr || out[0] == 0)
         return fail(EIO);
      uint64_t seq = out[0];
      __atomic_store_n(&kfifo_done, out[1], __ATOMIC_RELEASE);
      if (out[2] >= kfifo_gpu_us) {
         st.gpu_us += out[2] - kfifo_gpu_us;
         kfifo_gpu_us = out[2];
      }
      submits++;
      st.submits++;
      st.submit_ns += t1 - t0;
      stats_tick(t1);
      const uint32_t *handles = (const uint32_t *)(uintptr_t)s->bo_handles;
      for (uint32_t i = 0; i < s->bo_handle_count; i++)
         if (handles[i] && handles[i] <= MAX_BOS)
            bo_last_job[handles[i]] = seq;
      return 0;
   }
   if (!async_on)
      return submit_now(s->jc, js);
   /* This thread's write-combined writes of the chain land before another core starts the job. */
   __asm__ volatile ("dsb sy" ::: "memory");
   pthread_mutex_lock(&qlock);
   while (queued - finished == QUEUE_SIZE)
      pthread_cond_wait(&qdone, &qlock);
   queue[queued % QUEUE_SIZE].jc = s->jc;
   queue[queued % QUEUE_SIZE].js = js;
   queued++;
   /* the job's BOs are busy until this job is finished (Mesa frees the list when this returns) */
   const uint32_t *handles = (const uint32_t *)(uintptr_t)s->bo_handles;
   for (uint32_t i = 0; i < s->bo_handle_count; i++)
      if (handles[i] && handles[i] <= MAX_BOS)
         bo_last_job[handles[i]] = queued;
   pthread_cond_signal(&qwork);
   pthread_mutex_unlock(&qlock);
   return 0;
}

EXPORT int
drmIoctl(int fd, unsigned long request, void *arg)
{
   (void)fd;
   int r = 0;
   pthread_mutex_lock(&lock);
   if (connect_gpu()) {
      pthread_mutex_unlock(&lock);
      return fail(ENODEV);
   }
   switch (request) {
   case DRM_IOCTL_PANFROST_GET_PARAM: {
      struct drm_panfrost_get_param *p = arg;
      uint64_t in = p->param, out = 0;
      if (call(SEL_PARAM, &in, 1, &out, 1))
         r = fail(EINVAL);
      else {
         /*
          * No ASTC (HERO_ASTC=1 keeps it): iOS's own wallpapers come as ASTC
          * 4x4 and stayed black, the T880 taking them without an error. Without
          * the bits (20-23: ASTC 3D/2D, LDR/HDR) Panfrost says it has no ASTC
          * and Mesa decodes every such picture on the CPU as it is uploaded.
          */
         if (p->param == DRM_PANFROST_PARAM_TEXTURE_FEATURES0) {
            const char *a = getenv("HERO_ASTC");
            if (!a || *a != '1') {
               herodrm_log("texture features %#llx: ASTC hidden, Mesa decodes it", (unsigned long long)out);
               out &= ~(0xfull << 20);
            }
         }
         p->value = out;
      }
      break;
   }
   case DRM_IOCTL_PANFROST_CREATE_BO:
      r = create_bo(arg);
      break;
   case DRM_IOCTL_GEM_CLOSE:
      r = free_bo(((struct drm_gem_close *)arg)->handle);
      break;
   case DRM_IOCTL_PANFROST_GET_BO_OFFSET: {
      struct drm_panfrost_get_bo_offset *p = arg;
      struct hbo *bo = lookup(p->handle);
      if (bo)
         p->offset = bo->va;
      else
         r = fail(EINVAL);
      break;
   }
   case DRM_IOCTL_PANFROST_MMAP_BO: {
      struct drm_panfrost_mmap_bo *p = arg;
      if (lookup(p->handle))
         p->offset = (uint64_t)p->handle << 32;
      else
         r = fail(EINVAL);
      break;
   }
   case DRM_IOCTL_PANFROST_WAIT_BO:
      /* Only the jobs that named this BO: an idle one can be reused at once from Mesa's cache. */
      if (((struct drm_panfrost_wait_bo *)arg)->timeout_ns == 0 &&
          bo_busy(((struct drm_panfrost_wait_bo *)arg)->handle))
         r = fail(ETIMEDOUT);
      else
         bo_wait(((struct drm_panfrost_wait_bo *)arg)->handle);
      break;
   case DRM_IOCTL_PANFROST_MADVISE: {
      struct drm_panfrost_madvise *m = arg;
      if (!m->handle || m->handle > MAX_BOS || !bos[m->handle].in_use) {
         r = fail(EINVAL);
         break;
      }
      struct hbo *bo = &bos[m->handle];
      if (bo->purged) {
         m->retained = 0;
      } else if (m->madv == PANFROST_MADV_DONTNEED) {
         if (!bo->evictable) {
            bo->evictable = true;
            evictable_bytes += bo->size;
         }
         if (evictable_bytes > (96u * 1024u * 1024u)) {
            purge_evictable(64u * 1024u * 1024u, true);
            if (evictable_bytes > (128u * 1024u * 1024u))
               purge_evictable(64u * 1024u * 1024u, false);
         }
         m->retained = bo->purged ? 0 : 1;
      } else {
         if (bo->evictable) {
            bo->evictable = false;
            evictable_bytes -= bo->size;
         }
         m->retained = 1;
      }
      break;
   }
   case DRM_IOCTL_PANFROST_SUBMIT:
      r = submit(arg);
      break;
   default:
      herodrm_log("unhandled ioctl 0x%lx", request);
      r = fail(EINVAL);
      break;
   }
   pthread_mutex_unlock(&lock);
   return r;
}

/* mmap() of a BO: offset is handle << 32 plus the offset inside the BO. */
EXPORT void *
herodrm_mmap(void *addr, size_t size, int prot, int flags, int fd, off_t offset)
{
   (void)addr;
   (void)prot;
   (void)flags;
   (void)fd;
   uint32_t handle = (uint32_t)((uint64_t)offset >> 32);
   uint64_t inner = (uint64_t)offset & 0xffffffffull;
   void *p = MAP_FAILED;
   pthread_mutex_lock(&lock);
   struct hbo *bo = lookup(handle);
   if (bo && inner + size <= bo->size) {
      if (!bo->cpu) {
         uint64_t in = bo->khandle, out = 0;
         if (call(SEL_MAP, &in, 1, &out, 1) == 0)
            bo->cpu = out;
      }
      if (bo->cpu)
         p = (void *)(uintptr_t)(bo->cpu + inner);
   }
   pthread_mutex_unlock(&lock);
   if (p == MAP_FAILED) {
      herodrm_log("mmap of BO %u (+0x%llx, %zu bytes) failed", handle,
                  (unsigned long long)inner, size);
      errno = EINVAL;
   }
   return p;
}

/* The kernel keeps one mapping per BO until it is freed. */
EXPORT int
herodrm_munmap(void *addr, size_t size)
{
   (void)addr;
   (void)size;
   return 0;
}

EXPORT int
drmCloseBufferHandle(int fd, uint32_t handle)
{
   (void)fd;
   pthread_mutex_lock(&lock);
   int r = free_bo(handle);
   pthread_mutex_unlock(&lock);
   return r;
}

/*
 * IOSurfaces as GPU buffers (kernel 0248, HeroGPU SurfaceBO): Panfrost
 * imports foreign memory only as a dma-buf file descriptor. A surface import
 * gets a /dev/null descriptor that stands for its BO: drmPrimeFDToHandle
 * gives the handle and herodrm_prime_size() the size (Mesa's
 * pan_kmod_bo_import is patched to ask it instead of lseek on Apple).
 */
struct herodrm_surface {
   uint32_t handle, bytes_per_row, width, height, pixel_format, offset, planes;
   uint64_t size;
};

EXPORT int
herodrm_surface_import(uint32_t surface_id, struct herodrm_surface *info)
{
   int r = -1;
   pthread_mutex_lock(&lock);
   if (connect_gpu() == 0) {
      uint64_t in = surface_id, out[9] = {0};
      uint32_t kr = call(SEL_SURFACE_BO, &in, 1, out, 9);
      if (kr != 0 || !out[0] || out[0] > MAX_BOS) {
         if (purge_evictable(0, false) > 0)
            kr = call(SEL_SURFACE_BO, &in, 1, out, 9);
      }
      if (kr == 0 && out[0] && out[0] <= MAX_BOS) {
         int slot = -1;
         for (int i = 0; i < MAX_PRIME && slot < 0; i++)
            if (!primes[i].handle)
               slot = i;
         uint32_t u = alloc_uhandle();
         int fd = (slot >= 0 && u != 0) ? open("/dev/null", O_RDONLY | O_CLOEXEC) : -1;
         if (fd >= 0) {
            bos[u] = (struct hbo){ .khandle = (uint32_t)out[0], .va = out[1], .size = out[2], .in_use = true };
            primes[slot].fd = fd;
            primes[slot].handle = u;
            primes[slot].surface_id = surface_id;
            *info = (struct herodrm_surface){ .handle = u, .size = out[2],
               .bytes_per_row = (uint32_t)out[3], .width = (uint32_t)out[4], .height = (uint32_t)out[5],
               .pixel_format = (uint32_t)out[6], .offset = (uint32_t)out[7], .planes = (uint32_t)out[8] };
            r = fd;
            DBG("surface %u imported: BO %u (k%u) at 0x%llx, %ux%u bpr %u fmt 0x%x", surface_id,
                u, (unsigned)out[0], (unsigned long long)out[1], info->width, info->height,
                info->bytes_per_row, info->pixel_format);
         } else {
            in = out[0];
            call(SEL_FREE, &in, 1, NULL, 0);
         }
      } else if (debug > 0) {
         herodrm_log("surface %u: no GPU import (kr 0x%x)", surface_id, kr);
      }
   }
   pthread_mutex_unlock(&lock);
   return r < 0 ? fail(ENOENT) : r;
}

/* Make the GPU's writes into an imported surface visible to the CPU and the display. */
EXPORT int
herodrm_bo_sync(uint32_t handle)
{
   pthread_mutex_lock(&lock);
   queue_drain();
   struct hbo *bo = lookup(handle);
   uint64_t in = bo ? bo->khandle : 0;
   uint32_t kr = (conn && in) ? call(SEL_SYNC_BO, &in, 1, NULL, 0) : 1;
   pthread_mutex_unlock(&lock);
   return kr ? fail(EINVAL) : 0;
}

/* Clean CPU D-cache over an imported input IOSurface for the GPU WITHOUT stalling the Mali pipeline. */
EXPORT int
herodrm_surface_clean(uint32_t surface_id)
{
   if (!surface_id)
      return 0;
   pthread_mutex_lock(&lock);
   uint64_t in = 0;
   for (int i = 0; i < MAX_PRIME; i++) {
      if (primes[i].handle && primes[i].surface_id == surface_id) {
         struct hbo *bo = lookup(primes[i].handle);
         if (bo)
            in = bo->khandle;
         break;
      }
   }
   uint32_t kr = (conn && in) ? call(SEL_SYNC_BO, &in, 1, NULL, 0) : 0;
   pthread_mutex_unlock(&lock);
   return kr ? fail(EINVAL) : 0;
}

/* Release the kernel GPU VA + SweetSurface ref for an imported IOSurface whose GL texture was deleted. */
EXPORT void
herodrm_surface_forget(uint32_t surface_id)
{
   if (!surface_id)
      return;
   pthread_mutex_lock(&lock);
   for (int i = 0; i < MAX_PRIME; i++) {
      if (primes[i].handle && primes[i].surface_id == surface_id) {
         uint32_t u = primes[i].handle;
         close(primes[i].fd);
         primes[i].handle = 0;
         primes[i].surface_id = 0;
         if (u <= MAX_BOS && bos[u].in_use && !bos[u].purged && bos[u].khandle) {
            bo_wait(u);
            bo_last_job[u] = 0;
            uint64_t in = bos[u].khandle;
            call(SEL_FREE, &in, 1, NULL, 0);
            st.frees++;
            bos[u].khandle = 0;
            bos[u].va = 0;
            bos[u].size = 0;
            bos[u].cpu = 0;
            bos[u].purged = true;
         }
      }
   }
   pthread_mutex_unlock(&lock);
}

EXPORT int64_t
herodrm_prime_size(int prime_fd)
{
   int64_t size = -1;
   pthread_mutex_lock(&lock);
   for (int i = 0; i < MAX_PRIME; i++)
      if (primes[i].handle && primes[i].fd == prime_fd && lookup(primes[i].handle))
         size = (int64_t)bos[primes[i].handle].size;
   pthread_mutex_unlock(&lock);
   return size;
}

/* No export: nothing is shared with another process. */
EXPORT int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
   (void)fd; (void)handle; (void)flags; (void)prime_fd;
   return fail(ENOSYS);
}

EXPORT int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
   (void)fd;
   int r = fail(ENOENT);
   pthread_mutex_lock(&lock);
   for (int i = 0; i < MAX_PRIME; i++)
      if (primes[i].handle && primes[i].fd == prime_fd) {
         *handle = primes[i].handle;
         r = 0;
      }
   pthread_mutex_unlock(&lock);
   return r;
}

/* Syncobjs: submits are synchronous, so every syncobj is signaled. */
static uint32_t next_syncobj = 1;

EXPORT int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
   (void)fd; (void)flags;
   pthread_mutex_lock(&lock);
   *handle = next_syncobj++;
   pthread_mutex_unlock(&lock);
   return 0;
}

EXPORT int
drmSyncobjDestroy(int fd, uint32_t handle)
{
   (void)fd; (void)handle;
   return 0;
}

EXPORT int
drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec,
               unsigned flags, uint32_t *first_signaled)
{
   (void)fd; (void)handles; (void)num_handles; (void)flags;
   pthread_mutex_lock(&lock);
   kfifo_flush_batch();
   if (kfifo_on) {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      int64_t now_mono = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
      if (timeout_nsec <= now_mono) {
         pthread_mutex_unlock(&lock);
         if (first_signaled)
            *first_signaled = 0;
         return 0;
      }
   }
   queue_drain();
   pthread_mutex_unlock(&lock);
   if (first_signaled)
      *first_signaled = 0;
   return 0;
}

EXPORT int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles,
                       int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
   (void)points;
   return drmSyncobjWait(fd, handles, num_handles, timeout_nsec, flags, first_signaled);
}

EXPORT int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{
   (void)fd; (void)handles; (void)handle_count;
   return 0;
}

EXPORT int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count)
{
   (void)fd; (void)handles; (void)handle_count;
   return 0;
}

EXPORT int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle,
                   uint64_t src_point, uint32_t flags)
{
   (void)fd; (void)dst_handle; (void)dst_point; (void)src_handle; (void)src_point; (void)flags;
   return 0;
}

/* A sync file is a /dev/null descriptor: already signaled. (Mesa treats
 * -1 as a failed export and makes no fence.) */
EXPORT int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
   (void)fd; (void)handle;
   *sync_file_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
   return *sync_file_fd < 0 ? -1 : 0;
}

EXPORT int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
   (void)fd; (void)handle; (void)sync_file_fd;
   return 0;
}

EXPORT int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
   (void)obj_fd;
   return drmSyncobjCreate(fd, 0, handle);
}

EXPORT int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
   (void)fd; (void)handle;
   *obj_fd = -1;
   return fail(ENOSYS);
}
