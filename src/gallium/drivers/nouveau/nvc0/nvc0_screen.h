#ifndef __NVC0_SCREEN_H__
#define __NVC0_SCREEN_H__

#include "util/u_dynarray.h"

#include "nouveau_screen.h"
#include "nouveau_mm.h"
#include "nouveau_fence.h"
#include "nouveau_heap.h"

#include "nv_object.xml.h"

#include "nvc0/nvc0_winsys.h"
#include "nvc0/nvc0_stateobj.h"

#define NVC0_TIC_MAX_ENTRIES 2048
#define NVC0_TSC_MAX_ENTRIES 2048
#define NVE4_IMG_MAX_HANDLES 512

/* bindless handle i owns TIC and TSC slot NVC0_BINDLESS_SLOT(i) of every context,
 * a Kepler image handle (i < NVE4_IMG_MAX_HANDLES) bindless info block i */
#define NVC0_BINDLESS_MAX 1024
#define NVC0_BINDLESS_SLOT(i) (NVC0_TIC_MAX_ENTRIES - NVC0_BINDLESS_MAX + (i))

/* doesn't count driver-reserved slot */
#define NVC0_MAX_PIPE_CONSTBUFS 15
#define NVC0_MAX_CONST_BUFFERS  16
#define NVC0_MAX_CONSTBUF_SIZE  65536

#define NVC0_MAX_VIEWPORTS 16

#define NVC0_MAX_BUFFERS 32

#define NVC0_MAX_IMAGES 8

#define NVC0_MAX_WINDOW_RECTANGLES 8

struct nvc0_context;

struct nvc0_blitter;

struct nvc0_graph_state {
   /* kicked since the 3D/compute bufctx was last fenced */
   bool flushed_3d;
   bool flushed_cp;
   bool rasterizer_discard;
   bool early_z_forced;
   bool prim_restart;
   uint32_t instance_elts; /* bitmask of per-instance elements */
   uint32_t instance_base;
   uint32_t constant_vbos;
   uint32_t constant_elts;
   int32_t index_bias;
   uint16_t scissor;
   bool flatshade;
   uint8_t patch_vertices;
   uint8_t vbo_mode; /* 0 = normal, 1 = translate, 3 = translate, forced */
   uint8_t num_vtxbufs;
   uint8_t num_vtxelts;
   uint8_t num_textures[6];
   uint8_t num_samplers[6];
   uint8_t tls_required; /* bitmask of shader types using l[] */
   uint8_t clip_enable;
   uint32_t clip_mode;
   bool uniform_buffer_bound[6];
   struct nvc0_transform_feedback_state *tfb;
   bool seamless_cube_map;
   bool post_depth_coverage;
};

struct nvc0_cb_binding {
   uint64_t addr;
   int size;
};

/* what every context's channel shares, see nvc0_screen_init_context() */
struct nvc0_screen {
   struct nouveau_screen base;

   /* see nvc0_flush_resource() */
   simple_mtx_t storage_lock;
   unsigned storage_seq;

   /* channels of destroyed contexts, see nvc0_screen_put_channel() */
   simple_mtx_t chan_lock;
   struct util_dynarray chan_pool;

   struct nouveau_bo *tls;
   struct nouveau_bo *poly_cache;

   uint8_t gpc_count;
   uint16_t mp_count;
   uint16_t mp_count_compute; /* magic reg can make compute use fewer MPs */

   struct {
      bool maxwell;
   } tic;

   struct {
      struct nouveau_bo *bo;
      uint32_t *map;
      bool l2_flush; /* the GPU L2 caches system memory (GM20B) */
   } fence;

   struct {
      struct nvc0_hw_sm_query *mp_counter[8]; /* counter to query allocation */
      uint8_t num_hw_sm_active[2];
      bool mp_counters_enabled;
   } pm;

   uint32_t class_m2mf;
   uint32_t class_copy; /* 0 without a copy engine */
   uint32_t class_compute;

   struct {
      simple_mtx_t lock;
      struct nvc0_bindless *entries[NVC0_BINDLESS_MAX];
   } bindless;
};

/* a bindless handle, uploaded by each context that makes it resident */
struct nvc0_bindless {
   unsigned refcnt; /* the handle and each residency, under bindless.lock */
   struct pipe_resource *res;
   bool image; /* else a texture with a sampler */
   uint32_t tic[8];
   uint32_t tsc[8];
   int64_t tic_delta; /* TIC address - res address */
   struct pipe_image_view view; /* Kepler image handle */
};

static inline struct nvc0_screen *
nvc0_screen(struct pipe_screen *screen)
{
   return (struct nvc0_screen *)screen;
}

/* Maxwell's tiled caching (binned rasterization), undocumented method 0x0f60 */
static inline bool
nvc0_screen_has_tiled_cache(const struct nvc0_screen *screen)
{
   return screen->base.class_3d >= GM107_3D_CLASS &&
          screen->base.class_3d < GP100_3D_CLASS;
}

int nvc0_screen_get_driver_query_info(struct pipe_screen *, unsigned,
                                      struct pipe_driver_query_info *);

int nvc0_screen_get_driver_query_group_info(struct pipe_screen *, unsigned,
                                            struct pipe_driver_query_group_info *);

bool nvc0_blitter_create(struct nvc0_context *);
void nvc0_blitter_destroy(struct nvc0_context *);

void nvc0_screen_make_buffers_resident(struct nvc0_screen *);

int nvc0_screen_tic_alloc(struct nvc0_context *, void *);
int nvc0_screen_bindless_new(struct nvc0_screen *, struct nvc0_bindless *, bool kepler_image);
struct nvc0_bindless *nvc0_screen_bindless_get(struct nvc0_screen *, unsigned i);
void nvc0_screen_bindless_put(struct nvc0_screen *, unsigned i);
int nvc0_screen_tsc_alloc(struct nvc0_context *, void *);

int nve4_screen_compute_setup(struct nvc0_context *);
int nvc0_screen_compute_setup(struct nvc0_context *);

int nvc0_screen_init_context(struct nvc0_context *);
void nvc0_screen_fini_context(struct nvc0_context *);
int nvc0_screen_get_channel(struct nvc0_screen *, struct nouveau_object **);
void nvc0_screen_put_channel(struct nvc0_screen *, struct nouveau_object **);
int nvc0_screen_resize_text_area(struct nvc0_context *, uint64_t);

// 3D Only
void nvc0_screen_bind_cb_3d(struct nvc0_context *, bool *, int, int, int, uint64_t);

struct nvc0_format {
   uint32_t rt;
   struct {
      unsigned format:7;
      unsigned type_r:3;
      unsigned type_g:3;
      unsigned type_b:3;
      unsigned type_a:3;
      unsigned src_x:3;
      unsigned src_y:3;
      unsigned src_z:3;
      unsigned src_w:3;
   } tic;
   uint32_t usage;
};

struct nvc0_vertex_format {
   uint32_t vtx;
   uint32_t usage;
};

extern const struct nvc0_format nvc0_format_table[];
extern const struct nvc0_vertex_format nvc0_vertex_format[];

#endif
