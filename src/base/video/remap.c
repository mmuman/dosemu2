/*
 * DANG_BEGIN_MODULE
 *
 * REMARK
 * Transform a 2D image (rescale and color space conversion).
 *
 * Here are functions to adapt the VGA graphics to various
 * X displays.
 *
 * /REMARK
 * DANG_END_MODULE
 *
 * Copyright (c) 1997 Steffen Winterfeldt
 *
 *
 * DANG_BEGIN_CHANGELOG
 *
 * 1997/07/08: Gamma correction now uses only integer operations.
 * -- sw (Steffen Winterfeldt <wfeldt@suse.de>)
 *
 * 1998/11/01: Added so far unsupported video modes. We now
 * support all but the Hercules mode.
 * -- sw
 *
 * 1999/01/05: Added support for Hercules mode.
 * -- sw
 *
 * DANG_END_CHANGELOG
 *
 */

#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "vgaemu.h"
#include "mapping.h"
#include "render.h"
#include "render_priv.h"
#include "remap_priv.h"

#define LUT_OFS_33  256 * 3
#define LUT_OFS_67  256 * 4
#define LUT_OFS_11  256 * 5
#define LUT_OFS_22  256 * 6
#define LUT_OFS_45  256 * 7

#ifdef REMAP_TEST
RemapFuncDesc *remap_test(void);
#endif
RemapFuncDesc *remap_gen(void);

RemapFuncDesc *(*remap_list_funcs[])(void) = {
  remap_gen,
#if 0
#if defined(__i386__) && !defined(__clang__)
  remap_opt,
#endif
#endif
#ifdef REMAP_TEST
  remap_test,
#endif
  NULL
};

static RemapFuncDesc *remap_list = NULL;

static int base_init = 0;
static FILE *rdm = NULL;

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void do_base_init(void);

static void do_nothing(void) {};
static void do_nothing_remap(struct RemapObjectStruct *a) {};
static int do_nearly_nothing(RemapObject *a, unsigned b, unsigned c, unsigned d, unsigned e, unsigned f) { return 0; };
static RectArea do_nearly_something_rect(RemapObject *ro, int x0, int y0, int width, int height) { RectArea ra = {0, 0, 0, 0}; return ra; };
static RectArea do_nearly_something_mem(RemapObject *ro, int offset, int len) { RectArea ra = {0, 0, 0, 0}; return ra; };

static unsigned u_pow(unsigned, unsigned);
static unsigned gamma_fix(unsigned, unsigned);
static int true_col_palette_update(RemapObject *, unsigned, unsigned, unsigned, unsigned, unsigned);
static int pseudo_col_palette_update(RemapObject *, unsigned, unsigned, unsigned, unsigned, unsigned);

static void rgb_color_reduce(const ColorSpaceDesc *, int, int, int, RGBColor *);
static unsigned rgb_color_reduced_2int(const ColorSpaceDesc *, RGBColor);
static unsigned rgb_color_2int(const ColorSpaceDesc *csd, int rbits, int gbits,
    int bbits, RGBColor c);
static void rgb_lin_filt(RGBColor, RGBColor *, RGBColor *);
static void rgb_bilin_filt(RGBColor, RGBColor *, RGBColor *, RGBColor *);

static void src_resize_update(RemapObject *, int, int, int);
static void dst_resize_update(RemapObject *, int, int, int);
static void resize_update(RemapObject *);

int bre_s(int, int, int);
int bre_s2(int, int, int);
int bre_acc(int, int, int);
int bre_d_0(int, int, int);
void bre_update(RemapObject *);
void bre_lin_filt_update(RemapObject *);
void bre_bilin_filt_update(RemapObject *);

static RemapFuncDesc *find_remap_func(unsigned, int, int, RemapFuncDesc *);
static RemapFuncDesc *find_best_remap_func(unsigned, int, int, RemapFuncDesc *);
static void install_remap_funcs(RemapObject *, int);

static RectArea remap_mem_1(RemapObject *, int, int);
static RectArea remap_rect_1(RemapObject *, int, int, int, int);
static RectArea remap_rect_dst_1(RemapObject *, int, int, int, int);
static RectArea remap_mem_2(RemapObject *, int, int);

/*
 * set file handle for debug messages
 */
void set_remap_debug_msg(FILE *_rdm) { rdm = _rdm; }


/*
 * some basic intializations
 */
static void do_base_init(void)
{
  RemapFuncDesc *rfd0, *rfd;
  int i = 0;

  /* setup remap_list to hold a chained list of remap function descriptions */

  remap_list = NULL;

  while(remap_list_funcs[i] != NULL) {
    rfd0 = rfd = remap_list_funcs[i++]();
    if(rfd != NULL) {
      while(rfd->next != NULL) rfd = rfd->next;
      rfd->next = remap_list;
      remap_list = rfd0;
    }
  }
}


/*
 * initialize a remap object
 */
static RemapObject *_remap_init(int src_mode, int dst_mode, int features,
	const ColorSpaceDesc *color_space, int gamma)
{
  RemapObject *ro = malloc(sizeof(*ro));
  int color_lut_size = 256;
  unsigned u, u0, u1;

  ro->features = features;
  ro->palette_update = do_nearly_nothing;
  ro->src_resize = src_resize_update;
  ro->dst_resize = dst_resize_update;
  ro->remap_rect = do_nearly_something_rect;
  ro->remap_rect_dst = do_nearly_something_rect;
  ro->remap_mem = do_nearly_something_mem;
  ro->state = 0;
  ro->src_mode = src_mode;
  ro->dst_mode = dst_mode;
  ro->src_tmp_line = NULL;
  ro->src_width = ro->src_height = ro->src_scan_len =
  ro->dst_width = ro->dst_height = ro->dst_scan_len =
  ro->src_x0 = ro->src_y0 = ro->src_x1 = ro->src_y1 =
  ro->dst_x0 = ro->dst_y0 = ro->dst_x1 = ro->dst_y1 =
  ro->src_offset = ro->dst_offset = 0;
  ro->src_start = ro->dst_start = 0;
  ro->bre_x = ro->bre_y = NULL;
  ro->true_color_lut = NULL;
  ro->color_lut_size = 0;
  ro->bit_lut = NULL;
  ro->gamma_lut = malloc(256 * (sizeof *ro->gamma_lut));
  for(u = 0; u < 256; u++)
    ro->gamma_lut[u] = gamma_fix(u, gamma);
  ro->gamma = gamma;

  ro->remap_func = ro->remap_func_init = NULL;
  ro->remap_func_flags = 0;
  ro->remap_func_name = "no_func";
#if 0
  ro->co = malloc(sizeof(*ro->co));
  if(ro->co == NULL) {
    ro->state |= ROS_MALLOC_FAIL;
  }
  else {
    *ro->co = code_init();
  }
#endif
  ro->remap_line = NULL;
  ro->func_all = ro->func_1 = ro->func_2 = NULL;

  if(!base_init) {
    do_base_init();
    base_init = 1;
  }

  ro->dst_color_space = color_space;
  ro->dst_image = NULL;

  ro->bre_x = calloc(1, sizeof(*ro->bre_x));
  if(ro->bre_x == NULL) ro->state |= ROS_MALLOC_FAIL;
  ro->bre_y = calloc(1, sizeof(*ro->bre_y));
  if(ro->bre_y == NULL) ro->state |= ROS_MALLOC_FAIL;

  install_remap_funcs(ro, features);

  if(
    (ro->func_all && (ro->func_all->flags & RFF_LIN_FILT)) ||
    (ro->func_1 && (ro->func_1->flags & RFF_LIN_FILT)) ||
    (ro->func_2 && (ro->func_2->flags & RFF_LIN_FILT))
  ) {
    color_lut_size = 256 * 3;
  }

  if(
    (ro->func_all && (ro->func_all->flags & RFF_BILIN_FILT)) ||
    (ro->func_1 && (ro->func_1->flags & RFF_BILIN_FILT)) ||
    (ro->func_2 && (ro->func_2->flags & RFF_BILIN_FILT))
  ) {
    color_lut_size = 256 * 6;
  }

  if(
    (
      ro->src_mode & (
        MODE_PSEUDO_8 | MODE_VGA_X |
        MODE_VGA_1 | MODE_VGA_2 | MODE_VGA_4 |
        MODE_CGA_1 | MODE_CGA_2 | MODE_HERC
      )
    ) == ro->src_mode
  ) {
    if((ro->dst_mode & MODE_PSEUDO_8) == ro->dst_mode) {
      ro->palette_update = do_nearly_nothing;
    }
    if((ro->dst_mode & MODE_TRUE_8) == ro->dst_mode) {
      ro->true_color_lut = calloc(color_lut_size, sizeof(*ro->true_color_lut));
      if(ro->true_color_lut == NULL) ro->state |= ROS_MALLOC_FAIL;
      ro->color_lut_size = color_lut_size;
      ro->palette_update = pseudo_col_palette_update;
    }
    else if((ro->dst_mode & MODE_TRUE_COL) == ro->dst_mode) {
      /* **** really too much for now...  **** */
      color_lut_size *= 3;
      ro->true_color_lut = calloc(color_lut_size, sizeof(*ro->true_color_lut));
      if(ro->true_color_lut == NULL) ro->state |= ROS_MALLOC_FAIL;
      ro->color_lut_size = color_lut_size;
      ro->palette_update = true_col_palette_update;
    }
    ro->bit_lut = calloc(8*4*256, 1);
    if(ro->bit_lut == NULL) {
      ro->state |= ROS_MALLOC_FAIL;
    }
    else {
      for(u = 0; u < 0x100; u++) {
        u0 = u1 = 0;
        if((u & 0x80)) u0 |= 1 <<  0;
        if((u & 0x40)) u0 |= 1 <<  8;
        if((u & 0x20)) u0 |= 1 << 16;
        if((u & 0x10)) u0 |= 1 << 24;
        if((u & 0x08)) u1 |= 1 <<  0;
        if((u & 0x04)) u1 |= 1 <<  8;
        if((u & 0x02)) u1 |= 1 << 16;
        if((u & 0x01)) u1 |= 1 << 24;
        ro->bit_lut[2 * u            ] = u0;
        ro->bit_lut[2 * u + 1        ] = u1;
        ro->bit_lut[2 * u     + 0x200] = u0 << 1;
        ro->bit_lut[2 * u + 1 + 0x200] = u1 << 1;
        ro->bit_lut[2 * u     + 0x400] = u0 << 2;
        ro->bit_lut[2 * u + 1 + 0x400] = u1 << 2;
        ro->bit_lut[2 * u     + 0x600] = u0 << 3;
        ro->bit_lut[2 * u + 1 + 0x600] = u1 << 3;
      }
    }
  }

  if(
    (ro->src_mode &
      (
        MODE_TRUE_COL | MODE_PSEUDO_8 | MODE_VGA_X |
        MODE_VGA_1 | MODE_VGA_2 | MODE_VGA_4
      )
    ) &&
    (ro->dst_mode & (MODE_TRUE_COL | MODE_PSEUDO_8)) == ro->dst_mode
  ) {
    ro->remap_mem = remap_mem_1;
    ro->remap_rect = remap_rect_1;
    ro->remap_rect_dst = remap_rect_dst_1;
  }

  if((ro->src_mode & (MODE_CGA_1 | MODE_CGA_2 | MODE_HERC)) &&
    (ro->dst_mode & (MODE_TRUE_COL | MODE_PSEUDO_8)) == ro->dst_mode
  ) {
    ro->remap_mem = remap_mem_2;
  }

  return ro;
}


/*
 * destroy a remap object
 */
#define FreeIt(_p_) if(_p_ != NULL) { free(_p_); _p_ = NULL; }
static void _remap_done(RemapObject *ro)
{
  FreeIt(ro->gamma_lut)
  FreeIt(ro->bre_x)
  FreeIt(ro->bre_y)
  FreeIt(ro->true_color_lut)
  FreeIt(ro->bit_lut);
  FreeIt(ro->src_tmp_line);
#if 0
  if(ro->co != NULL) {
    code_done(ro->co);
    free(ro->co);
    ro->co = NULL;
  }
#endif
  free(ro);
}
#undef FreeIt


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
/*
 * Calculate a^b (a >= 0, b >= 0).
 * a, b are fixed point numbers with 16 bit decimals.
 *
 * The result is accurate enough for our purpose (relative
 * error typically 10^-4, worst cases still better than 10^-2).
 */

unsigned u_pow(unsigned a, unsigned b)
{
  unsigned long long l, l2, r;
  unsigned b0;
  int i, j;

  if(a == 0) return b ? 0 : 1 << 16;

  b0 = b >> 16;
  r = 1 << 16;	/* 1.0 */

  for(l = a, i = 0; i < 16 && b0; i++) {
    if(b0 & 1) r = r * l >> 16;
    l = l * l >> 16;
    b0 >>= 1;
  }

  for(l = a, i = 0; i < 16 && (b & ((1 << 16) - 1)); i++) {
    for(l2 = l, j = 0; j < 10; j++) {
      l2 = (l2 >> 1) + (l << 15) / l2;
    }	/* l2 = sqrt(l) now */
    l = l2;
    if(b & (1 << 15)) r = r * l >> 16;
    b <<= 1;
  }

  return r;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
unsigned gamma_fix(unsigned color, unsigned gamma)
{
  return gamma ? u_pow(color << 8, (100 << 16) / gamma) >> 8 : color;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
void gamma_correct(RemapObject *ro, RGBColor *c, unsigned *bits)
{
  int i;

  if(*bits <= 1) return;
  if(ro->gamma_lut == NULL) return;
  i = *bits - 8;
  if(i > 0) {
    c->r >>= i; c->g >>= i; c->b >>= i;
  }
  if(i < 0) {
    i = -i;
    c->r <<= i; c->g <<= i; c->b <<= i;
  }
  c->r &= 255; c->g &= 255; c->b &= 255;
  c->r = ro->gamma_lut[c->r];
  c->g = ro->gamma_lut[c->g];
  c->b = ro->gamma_lut[c->b];
  *bits = 8;
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static int true_col_palette_update(RemapObject *ro, unsigned i, unsigned bits,
                                   unsigned r, unsigned g, unsigned b)
{
  RGBColor c = {r, g, b}, c1, c2, c3;
  unsigned u, u0, uo;

  if(i >= 256) return 0;

  gamma_correct(ro, &c, &bits);

  u0 = u = rgb_color_2int(ro->dst_color_space, bits, bits, bits, c);

  if(ro->dst_color_space->bits == 8) u |= u << 8;
  if(ro->dst_color_space->bits <= 16) u |= u << 16;

#if 0
  fprintf(rdm, "true_col_palette_update: pal[%u] = (0x%x, 0x%x, 0x%x) = 0x%08x\n", i, r, g, b, u);
#endif

  uo = ro->true_color_lut[i];
  ro->true_color_lut[i] = u;

  if(
    (ro->func_all && (ro->func_all->flags & RFF_LIN_FILT)) ||
    (ro->func_1 && (ro->func_1->flags & RFF_LIN_FILT)) ||
    (ro->func_2 && (ro->func_2->flags & RFF_LIN_FILT)) ||
    (ro->func_all && (ro->func_all->flags & RFF_BILIN_FILT)) ||
    (ro->func_1 && (ro->func_1->flags & RFF_BILIN_FILT)) ||
    (ro->func_2 && (ro->func_2->flags & RFF_BILIN_FILT))
  ) {
    rgb_color_reduce(ro->dst_color_space, bits, bits, bits, &c);
    rgb_lin_filt(c, &c1, &c2);
    ro->true_color_lut[i + LUT_OFS_33] = rgb_color_reduced_2int(ro->dst_color_space, c1);
    ro->true_color_lut[i + LUT_OFS_67] = rgb_color_reduced_2int(ro->dst_color_space, c2);
  }

  if(
    (ro->func_all && (ro->func_all->flags & RFF_BILIN_FILT)) ||
    (ro->func_1 && (ro->func_1->flags & RFF_BILIN_FILT)) ||
    (ro->func_2 && (ro->func_2->flags & RFF_BILIN_FILT))
  ) {
    rgb_bilin_filt(c, &c1, &c2, &c3);
    ro->true_color_lut[i + LUT_OFS_11] = rgb_color_reduced_2int(ro->dst_color_space, c1);
    ro->true_color_lut[i + LUT_OFS_22] = rgb_color_reduced_2int(ro->dst_color_space, c2);
    ro->true_color_lut[i + LUT_OFS_45] = rgb_color_reduced_2int(ro->dst_color_space, c3);
  }

  if(ro->dst_color_space->bits > 8 && ro->dst_color_space->bits <= 16) {
    ro->true_color_lut[i + 256] = u0;
    ro->true_color_lut[i + 256 * 2] = u0 << 16;
  }

  return uo == u ? 0 : 1;
}

static int pseudo_col_palette_update(RemapObject *ro, unsigned i, unsigned bits,
                                   unsigned r, unsigned g, unsigned b)
{
  RGBColor c = {r, g, b};
  unsigned u, uo;

  gamma_correct(ro, &c, &bits);

  u = rgb_color_2int(ro->dst_color_space, bits, bits, bits, c);

#if 0
  fprintf(rdm, "pseudo_col_palette_update: pal[%u] = (0x%x, 0x%x, 0x%x) = 0x%02x:0x%02x\n", i, r, g, b, u0, u1);
#endif

  if(i < 256) {
    uo = ro->true_color_lut[i];
    ro->true_color_lut[i] = u;
    return u == uo ? 0 : 1;
  }

  return 0;
}

static unsigned dit_col(int s_c, int d_c, int col, int dit, int lim)
{
  int k, l, k0, k1, kr;

  k0 = ((d_c - 1) * col + d_c - 2) / (s_c - 1);
  k1 = k0 + 1;
  if(k1 >= d_c) k1 = k0;
  k = ((s_c - 1) * k0) / (d_c - 1);
  l = ((s_c - 1) * k1) / (d_c - 1);
  if(k != l) {
    kr = ((col - k) * dit  + ((l - k) >> 1) ) / (l - k);
  }
  else {
    kr = 0;
  }

  return kr <= lim ? k0 : k1;		/* or < ? */
}

void rgb_color_reduce(const ColorSpaceDesc *csd, int rbits, int gbits,
    int bbits, RGBColor *c)
{
  c->r &= (1 << rbits) - 1;
  c->g &= (1 << gbits) - 1;
  c->b &= (1 << bbits) - 1;

  if(csd->r_mask || csd->g_mask || csd->b_mask) {
    c->r = csd->r_bits >= rbits ? c->r << (csd->r_bits - rbits) : c->r >> (rbits - csd->r_bits);
    c->g = csd->g_bits >= gbits ? c->g << (csd->g_bits - gbits) : c->g >> (gbits - csd->g_bits);
    c->b = csd->b_bits >= bbits ? c->b << (csd->b_bits - bbits) : c->b >> (bbits - csd->b_bits);
  }
}

unsigned rgb_color_reduced_2int(const ColorSpaceDesc *csd, RGBColor c)
{
  c.r <<= csd->r_shift;
  c.g <<= csd->g_shift;
  c.b <<= csd->b_shift;
  return c.r | c.g | c.b;
}

static unsigned rgb_color_2int(const ColorSpaceDesc *csd, int rbits, int gbits,
    int bbits, RGBColor c)
{
  unsigned r, g, b;
  unsigned i0, i1, i2, i3;

  if(csd->r_mask || csd->g_mask || csd->b_mask) {
    rgb_color_reduce(csd, rbits, gbits, bbits, &c);
    return rgb_color_reduced_2int(csd, c);
  }

  c.r &= (1 << rbits) - 1;
  c.g &= (1 << gbits) - 1;
  c.b &= (1 << bbits) - 1;

#ifdef REMAP_REAL_DITHER
  if(csd->r_bits && csd->g_bits && csd->b_bits) {
    r = dit_col(1 << rbits, csd->r_bits, c.r, 5, 1);
    g = dit_col(1 << gbits, csd->g_bits, c.g, 5, 1);
    b = dit_col(1 << bbits, csd->b_bits, c.b, 5, 1);
    i0 = r * csd->r_shift + g * csd->g_shift + b * csd->b_shift;

    r = dit_col(1 << rbits, csd->r_bits, c.r, 5, 3);
    g = dit_col(1 << gbits, csd->g_bits, c.g, 5, 3);
    b = dit_col(1 << bbits, csd->b_bits, c.b, 5, 3);
    i1 = r * csd->r_shift + g * csd->g_shift + b * csd->b_shift;

    r = dit_col(1 << rbits, csd->r_bits, c.r, 5, 4);
    g = dit_col(1 << gbits, csd->g_bits, c.g, 5, 4);
    b = dit_col(1 << bbits, csd->b_bits, c.b, 5, 4);
    i2 = r * csd->r_shift + g * csd->g_shift + b * csd->b_shift;

    r = dit_col(1 << rbits, csd->r_bits, c.r, 5, 2);
    g = dit_col(1 << gbits, csd->g_bits, c.g, 5, 2);
    b = dit_col(1 << bbits, csd->b_bits, c.b, 5, 2);
    i3 = r * csd->r_shift + g * csd->g_shift + b * csd->b_shift;

    if(csd->pixel_lut != NULL) {
      i0 = csd->pixel_lut[i0];
      i1 = csd->pixel_lut[i1];
      i2 = csd->pixel_lut[i2];
      i3 = csd->pixel_lut[i3];
    }
    return i0 + (i1 << 8) + (i2 << 16) + (i3 << 24);
  }
#else
  /*
   * the following calculation is taken directly from X.c
   */
  if(csd->r_bits && csd->g_bits && csd->b_bits && bits) {
    r = (c.r * csd->r_bits) >> rbits;
    g = (c.g * csd->g_bits) >> gbits;
    b = (c.b * csd->b_bits) >> bbits;

    i0 = r * csd->r_shift + g * csd->g_shift + b * csd->b_shift;

    r = (((c.r + (c.r - ((r << rbits) / csd->r_bits))) * csd->r_bits) >> rbits);
    g = (((c.g + (c.g - ((g << gbits) / csd->g_bits))) * csd->g_bits) >> gbits);
    b = (((c.b + (c.b - ((b << bbits) / csd->b_bits))) * csd->b_bits) >> bbits);

    if(r >= csd->r_bits) r = csd->r_bits - 1;
    if(g >= csd->g_bits) g = csd->g_bits - 1;
    if(b >= csd->b_bits) b = csd->b_bits - 1;

    i1 = r * csd->r_shift + g * csd->g_shift + b * csd->b_shift;

    if(csd->pixel_lut != NULL) {
      i0 = csd->pixel_lut[i0];
      i1 = csd->pixel_lut[i1];
    }

    return i0 + (i1 << 8) + (i1 << 16) + (i0 << 24);
  }
#endif

  return 0;
}

static unsigned bgr_2int(const ColorSpaceDesc *csd, int rbits, int gbits,
    int bbits, unsigned bgr)
{
    RGBColor c = { bgr >> (bbits + gbits), bgr >> bbits, bgr };

    return rgb_color_2int(csd, rbits, gbits, bbits, c);
}

#if 0
static RGBColor int_2rgb_color(const ColorSpaceDesc *csd, unsigned bits, unsigned u)
{
  RGBColor c = {0, 0, 0};
  unsigned nr = u & csd->r_mask, ng = u & csd->g_mask, nb = u & csd->b_mask;

  if(csd->r_mask || csd->g_mask || csd->b_mask) {
    nr >>= csd->r_shift;
    ng >>= csd->g_shift;
    nb >>= csd->b_shift;

    c.r = csd->r_bits >= bits ? nr >> (csd->r_bits - bits) : nr << (bits - csd->r_bits);
    c.g = csd->g_bits >= bits ? ng >> (csd->g_bits - bits) : ng << (bits - csd->g_bits);
    c.b = csd->b_bits >= bits ? nb >> (csd->b_bits - bits) : nb << (bits - csd->b_bits);

    return c;
  }

  return c;
}
#endif

void rgb_lin_filt(RGBColor c, RGBColor *c1, RGBColor *c2)
{
  unsigned u;

  u = (c.r + 1) / 3; c1->r = u; c2->r = c.r - u;
  u = (c.g + 1) / 3; c1->g = u; c2->g = c.g - u;
  u = (c.b + 1) / 3; c1->b = u; c2->b = c.b - u;
}

void rgb_bilin_filt(RGBColor c, RGBColor *c1, RGBColor *c2, RGBColor *c3)
{
  unsigned u;

#if 0
  u = c.r; c1->r = (u + 1) / 9; c2->r = (u * 2 + 1) / 9; c3->r = (u * 4 + 1) / 9;
  if(c1->r + 2 * c2->r + c3->r < u) c3->r++;
  if(c1->r + 2 * c2->r + c3->r < u) c2->r++;
  u = c.g; c1->g = (u + 1) / 9; c2->g = (u * 2 + 1) / 9; c3->g = (u * 4 + 1) / 9;
  if(c1->g + 2 * c2->g + c3->g < u) c3->g++;
  if(c1->g + 2 * c2->g + c3->g < u) c2->g++;
  u = c.b; c1->b = (u + 1) / 9; c2->b = (u * 2 + 1) / 9; c3->b = (u * 4 + 1) / 9;
  if(c1->b + 2 * c2->b + c3->b < u) c3->b++;
  if(c1->b + 2 * c2->b + c3->b < u) c2->b++;
#endif

  u = c.r; c1->r = (u + 4) / 9; c2->r = (u * 2 + 4) / 9; c3->r = u - c1->r - 2 * c2->r;
  u = c.g; c1->g = (u + 4) / 9; c2->g = (u * 2 + 4) / 9; c3->g = u - c1->g - 2 * c2->g;
  u = c.b; c1->b = (u + 4) / 9; c2->b = (u * 2 + 4) / 9; c3->b = u - c1->b - 2 * c2->b;

  /* magic !!! */
  if(c3->r == 114) c3->r--;
  if(c3->g == 114) c3->g--;
  if(c3->b == 114) c3->b--;

}



/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void src_resize_update(RemapObject *ro, int width, int height, int scan_len)
{
  if (  ro->src_width == width &&
	ro->src_height == height &&
	ro->src_scan_len == scan_len)
    return;
  ro->src_width = width;
  ro->src_height = height;
  ro->src_scan_len = scan_len;
  ro->src_tmp_line = realloc(ro->src_tmp_line, width);
  // check return value?
  resize_update(ro);
}

static void dst_resize_update(RemapObject *ro, int width, int height, int scan_len)
{
  if (  ro->dst_width == width &&
	ro->dst_height == height &&
	ro->dst_scan_len == scan_len)
    return;
  ro->dst_width = width;
  ro->dst_height = height;
  ro->dst_scan_len = scan_len;
  resize_update(ro);
}

static void resize_update(RemapObject *ro)
{
  ro->state &= ~(ROS_REMAP_FUNC_OK | ROS_REMAP_IGNORE);

  if(!(ro->state & (ROS_SCALE_ALL | ROS_SCALE_1 | ROS_SCALE_2))) {
    return;
  }

  if(
    ro->src_width == 0 || ro->src_height == 0 ||
    ro->dst_width == 0 || ro->dst_height == 0 ||
    ro->src_scan_len == 0 || ro->dst_scan_len == 0
  ) {
    ro->state |= ROS_REMAP_FUNC_OK | ROS_REMAP_IGNORE;
    return;
  }

  ro->remap_line = do_nothing;

  if(
    ro->src_width == ro->dst_width &&
    ro->src_height == ro->dst_height &&
    (ro->state & ROS_SCALE_1)
  ) {
    ro->remap_func = ro->func_1->func;
    ro->remap_func_flags = ro->func_1->flags;
    ro->remap_func_name = ro->func_1->func_name;
    ro->remap_func_init = ro->func_1->func_init;
  }
  else if(
    (ro->src_width << 1) == ro->dst_width &&
    (ro->src_height << 1) == ro->dst_height &&
    (ro->state & ROS_SCALE_2)
  ) {
    ro->remap_func = ro->func_2->func;
    ro->remap_func_flags = ro->func_2->flags;
    ro->remap_func_name = ro->func_2->func_name;
    ro->remap_func_init = ro->func_2->func_init;
  }
  else if(ro->state & ROS_SCALE_ALL) {
    ro->remap_func = ro->func_all->func;
    ro->remap_func_flags = ro->func_all->flags;
    ro->remap_func_name = ro->func_all->func_name;
    ro->remap_func_init = ro->func_all->func_init;
  }
  else {
    ro->remap_func = do_nothing_remap;
    ro->remap_func_flags = 0;
    ro->remap_func_name = "do_nothing";
    ro->remap_func_init = NULL;
  }

  if(ro->remap_func_flags & RFF_BILIN_FILT)
    bre_bilin_filt_update(ro);
  else if(ro->remap_func_flags & RFF_LIN_FILT)
    bre_lin_filt_update(ro);
  else
    bre_update(ro);

  if(ro->remap_func_init != NULL) {
    ro->remap_func_init(ro);
  }

  if(ro->remap_func != NULL && ro->remap_func != do_nothing_remap) { ro->state |= ROS_REMAP_FUNC_OK; }

#ifdef REMAP_RESIZE_DEBUG
  fprintf(rdm, "resize_update: using %s for remap %dx%d --> %dx%d\n",
    ro->remap_func_name, ro->src_width, ro->src_height, ro->dst_width, ro->dst_height
  );
#endif

}


int bre_s(int d, int s_len, int d_len)
{
  return (s_len / 2 + s_len * d) / d_len;
}

int bre_s2(int d, int s_len, int d_len)
{
  return (s_len * d) / d_len;
}

int bre_acc(int d, int s_len, int d_len)
{
  return (s_len / 2 + s_len * d) % d_len;
}

int bre_d_0(int s, int s_len, int d_len)
{
  return ((s_len + 1) / 2 - 1 + s * d_len) / s_len;
}

void bre_update(RemapObject *ro)
{
  int i, l, *ii, y;

  if(ro->bre_x != NULL) free(ro->bre_x);
  if(ro->bre_y != NULL) free(ro->bre_y);

  if(!(l = ro->dst_width)) l++;
  ro->bre_x = ii = malloc(l * sizeof(*ro->bre_x));

  if(ro->bre_x == NULL) {
    ro->state |= ROS_MALLOC_FAIL;
  }
  else {
    for(i = 0; i < l; i++) ii[i] = bre_s(i + 1, ro->src_width, l);

    if(ro->src_mode == MODE_VGA_X) {
      for(i = 0; i < l; i++) ii[i] = (ii[i] >> 2) + ((ii[i] & 3) << 16);
    }

    for(i = l - 1; i > 0; i--) ii[i] -= ii[i - 1];
  }

  if(!(l = ro->dst_height)) l++;
  ro->bre_y = ii = malloc(l * sizeof(*ro->bre_y));

  if(ro->bre_y == NULL) {
    ro->state |= ROS_MALLOC_FAIL;
  }
  else {
    for(i = 0; i < l; i++) {
      if(ro->src_mode == MODE_CGA_1 || ro->src_mode == MODE_CGA_2) {
        y = bre_s(i, ro->src_height, l);
        ii[i] = (y >> 1) * ro->src_scan_len + ((y & 1) ? 0x2000 : 0);
      }
      else if(ro->src_mode == MODE_HERC) {
        y = bre_s(i, ro->src_height, l);
        ii[i] = (y >> 2) * ro->src_scan_len + (y & 3) * 0x2000;
      }
      else {
        ii[i] = bre_s(i, ro->src_height, l) * ro->src_scan_len;
      }
    }
  }

}

void bre_lin_filt_update(RemapObject *ro)
{
  int i, k, l, *ii;

  if(ro->bre_x != NULL) free(ro->bre_x);
  if(ro->bre_y != NULL) free(ro->bre_y);

  if(!(l = ro->dst_width)) l++;
  ro->bre_x = ii = malloc(2 * l * sizeof(*ro->bre_x));

  if(ro->bre_x == NULL) {
    ro->state |= ROS_MALLOC_FAIL;
  }
  else {
    k = ro->src_width;
    if(k == 0) k = 1;
    k = 3 * (k - 1);
    for(i = 0; i < l; i++) { ii[i] = bre_s2(i + 1, k, l - 1); }
    for(ii[l] = 0, i = 1; i < l; i++) { ii[l + i] = ii[i - 1] % 3; }

    for(i = 0; i < l; i++) { ii[i] /= 3; }
    ii[l - 1] = ro->src_width - 1;		/* just for fun... (value is never needed) */
    for(i = l - 1; i > 0; i--) { ii[i] -= ii[i - 1]; }
    ii[0] = 0;
    if(ii[2 * l - 1] != 0) fprintf(stderr, "**** oho: %d remains\n", ii[2 * l - 1]);
  }

  if(!(l = ro->dst_height)) l++;
  ro->bre_y = ii = malloc(l * sizeof(*ro->bre_y));

  if(ro->bre_y == NULL) {
    ro->state |= ROS_MALLOC_FAIL;
  }
  else {
    for(i = 0; i < l; i++) {
      ii[i] = bre_s(i, ro->src_height, l) * ro->src_scan_len;
    }
  }
}

void bre_bilin_filt_update(RemapObject *ro)
{
  int i, k, l, *ii;

  if(ro->bre_x != NULL) free(ro->bre_x);
  if(ro->bre_y != NULL) free(ro->bre_y);

  if(!(l = ro->dst_width)) l++;
  ro->bre_x = ii = malloc(2 * l * sizeof(*ro->bre_x));

  if(ro->bre_x == NULL) {
    ro->state |= ROS_MALLOC_FAIL;
  }
  else {
    k = ro->src_width;
    if(k == 0) k = 1;
    k = 3 * (k - 1);
    for(i = 0; i < l; i++) { ii[i] = bre_s2(i + 1, k, l - 1); }
    for(ii[l] = 0, i = 1; i < l; i++) { ii[l + i] = ii[i - 1] % 3; }

    for(i = 0; i < l; i++) { ii[i] /= 3; }
    ii[l - 1] = ro->src_width - 1;		/* just for fun... (value is never needed) */
    for(i = l - 1; i > 0; i--) { ii[i] -= ii[i - 1]; }
    ii[0] = 0;
    if(ii[2 * l - 1] != 0) fprintf(stderr, "**** oho: %d remains\n", ii[2 * l - 1]);
  }

  if(!(l = ro->dst_height)) l++;
  ro->bre_y = ii = malloc(2 * l * sizeof(*ro->bre_y));

  if(ro->bre_y == NULL) {
    ro->state |= ROS_MALLOC_FAIL;
  }
  else {
    k = ro->src_height;
    if(k == 0) k = 1;
    k = 3 * (k - 1);

    for(i = 0; i < l; i++) { ii[i] = bre_s2(i, k, l - 1); }
    for(i = 0; i < l; i++) { ii[l + i] = ii[i] % 3; }

    for(i = 0; i < l; i++) { ii[i] /= 3; }
    if(ii[2 * l - 1] != 0) fprintf(stderr, "**** oho: %d remains\n", ii[2 * l - 1]);
    if(ii[l - 1] >= ro->src_height) fprintf(stderr, "**** oho: %d lines is out of bounds\n", ii[l - 1]);
    for(i = 0; i < l; i++) { ii[i] *= ro->src_scan_len; }
  }
}

#ifdef REMAP_AREA_DEBUG
#define REMAP_AREA_DEBUG_FUNC(_ro_) remap_area_debug_func(_ro_)

static void remap_area_debug_func(RemapObject *ro)
{
  fprintf(rdm, "[%s]\n", ro->remap_func_name);
  fprintf(rdm, "  src_offset = %d, src_scan_len = %d, src_area = %d x %d, %d x %d\n",
    ro->src_offset, ro->src_scan_len, ro->src_x0, ro->src_y0, ro->src_x1, ro->src_y1
  );
  fprintf(rdm, "  dst_offset = %d, dst_scan_len = %d, dst_area = %d x %d, %d x %d\n",
    ro->dst_offset, ro->dst_scan_len, ro->dst_x0, ro->dst_y0, ro->dst_x1, ro->dst_y1
  );
}

#else
#define REMAP_AREA_DEBUG_FUNC(_ro_)
#endif

static RectArea remap_mem_1(RemapObject *ro, int offset, int len)
{
  RectArea ra = {0, 0, 0, 0};
  int i1, i2, j1, j2;
  int pixel_size = 1;

  if(ro->state & ROS_REMAP_IGNORE) return ra;
  if(ro->remap_func == NULL) return ra;

#ifdef REMAP_AREA_DEBUG
  fprintf(rdm, "remap_mem: ofs = %d, len = %d\n", offset, len);
  fprintf(rdm,
    "  src: base = 0x%x, width = %d, height = %d, scan_len = %d\n",
    (unsigned) ro->src_image, ro->src_width, ro->src_height, ro->src_scan_len
  );
  fprintf(rdm,
    "  dst: base = 0x%x, width = %d, height = %d, scan_len = %d\n",
    (unsigned) ro->dst_image, ro->dst_width, ro->dst_height, ro->dst_scan_len
  );
#endif

  if(offset < 0) len += offset, offset = 0;

  switch(ro->src_mode) {
    case MODE_TRUE_15:
    case MODE_TRUE_16: pixel_size = 2; break;
    case MODE_TRUE_24: pixel_size = 3; break;
    case MODE_TRUE_32: pixel_size = 4; break;
    case MODE_TRUE_8:
    case MODE_PSEUDO_8:
    default: pixel_size = 1;
  }
  if (len < pixel_size) return ra;

  i1 = offset / ro->src_scan_len;
  i2 = (offset % ro->src_scan_len) / pixel_size;
  j1 = (offset + len) / ro->src_scan_len;
  j2 = ((offset + len) % ro->src_scan_len) / pixel_size;

  /* make sure it's all visible */
  if(i2 >= ro->src_width) i1++, i2 = 0, offset = i1 * ro->src_scan_len;
  if(i1 >= ro->src_height || i1 > j1) return ra;
  if(j2 >= ro->src_width) j1++, j2 = 0;
  if(j1 >= ro->src_height) j1 = ro->src_height, j2 = 0;

  ra.width = ro->dst_width;

  if (ro->remap_func_flags & (RFF_REMAP_RECT | RFF_REMAP_LINES)) {
    ro->src_offset = i1 * ro->src_scan_len;
    ro->src_x0 = ro->dst_x0 = 0;
    ro->src_x1 = ro->src_width;
    ro->dst_x1 = ro->dst_width;
    ro->src_y0 = i1;
    ro->src_y1 = j1;
    if(j2) ro->src_y1++;
    ro->dst_y0 = bre_d_0(ro->src_y0, ro->src_height, ro->dst_height);
    ro->dst_y1 = bre_d_0(ro->src_y1, ro->src_height, ro->dst_height);
    ro->dst_offset = ro->dst_y0 * ro->dst_scan_len;
    ra.y = ro->dst_y0;
    ra.height = ro->dst_y1 - ro->dst_y0;
    REMAP_AREA_DEBUG_FUNC(ro);
    if(ro->dst_y0 != ro->dst_y1) {
      ro->remap_func(ro);
    }
  }
  else {
    ro->src_offset = ro->dst_offset = 0;
    ro->src_x0 = ro->dst_x0 = ro->src_y0 = ro->dst_y0 = 0;
    ro->src_x1 = ro->src_width;
    ro->dst_x1 = ro->dst_width;
    ro->src_y1 = ro->src_height;
    ro->dst_y1 = ro->dst_height;
    ra.height = ro->dst_height;
    REMAP_AREA_DEBUG_FUNC(ro);
    ro->remap_func(ro);
  }

  return ra;
}


static RectArea remap_rect_1(RemapObject *ro, int x0, int y0, int width, int height)
{
  RectArea ra = {0, 0, 0, 0};
  int x1, y1;
  int pixel_size = 1;

  if(ro->state & ROS_REMAP_IGNORE) return ra;
  if(ro->remap_func == NULL) return ra;

  if(x0 < 0) width -= x0, x0 = 0;
  if(y0 < 0) height -= y0, y0 = 0;

  if(x0 >= ro->src_width || y0 >= ro->src_height) return ra;
  if(width <= 0 || height <= 0) return ra;

  x1 = x0 + width;
  y1 = y0 + height;

  if(x1 > ro->src_width) x1 = ro->src_width, width = x1 - x0;
  if(y1 > ro->src_height) y1 = ro->src_height, height = y1 - y0;

  ra.x = bre_d_0(x0, ro->src_width, ro->dst_width);
  ra.y = bre_d_0(y0, ro->src_height, ro->dst_height);
  ra.width = bre_d_0(x1, ro->src_width, ro->dst_width) - ra.x;
  ra.height = bre_d_0(y1, ro->src_height, ro->dst_height) - ra.y;

  if(!(ra.width && ra.height)) return ra;

  switch(ro->dst_mode) {
    case MODE_TRUE_15:
    case MODE_TRUE_16: pixel_size = 2; break;
    case MODE_TRUE_24: pixel_size = 3; break;
    case MODE_TRUE_32: pixel_size = 4; break;
    case MODE_TRUE_8:
    case MODE_PSEUDO_8:
    default: pixel_size = 1;
  }

  if(ro->remap_func_flags & RFF_REMAP_RECT) {
    ro->src_x0 = x0;
    ro->src_x1 = x1;
    ro->src_y0 = y0;
    ro->src_y1 = y1;
    ro->src_offset = ro->src_y0 * ro->src_scan_len + ro->src_x0;
    ro->dst_x0 = ra.x;
    ro->dst_x1 = ra.x + ra.width;
    ro->dst_y0 = ra.y;
    ro->dst_y1 = ra.y + ra.height;
    ro->dst_offset = ro->dst_y0 * ro->dst_scan_len + ro->dst_x0 * pixel_size;
    REMAP_AREA_DEBUG_FUNC(ro);
    ro->remap_func(ro);
  }
  else if(ro->remap_func_flags & RFF_REMAP_LINES) {
    ro->src_x0 = 0;
    ro->src_x1 = ro->src_width;
    ro->src_y0 = y0;
    ro->src_y1 = y1;
    ro->src_offset = ro->src_y0 * ro->src_scan_len;
    ro->dst_x0 = 0;
    ro->dst_x1 = ro->dst_width;
    ro->dst_y0 = ra.y;
    ro->dst_y1 = ra.y + ra.height;
    ro->dst_offset = ro->dst_y0 * ro->dst_scan_len;
    REMAP_AREA_DEBUG_FUNC(ro);
    ro->remap_func(ro);
  }
  else {
    ro->src_offset = ro->dst_offset = 0;
    ro->src_x0 = ro->dst_x0 = ro->src_y0 = ro->dst_y0 = 0;
    ro->src_x1 = ro->src_width;
    ro->dst_x1 = ro->dst_width;
    ro->src_y1 = ro->src_height;
    ro->dst_y1 = ro->dst_height;
    ra.x = ra.y = 0;
    ra.width = ro->dst_width;
    ra.height = ro->dst_height;
    REMAP_AREA_DEBUG_FUNC(ro);
    ro->remap_func(ro);
  }

  return ra;
}

static RectArea remap_rect_dst_1(RemapObject *ro, int x0, int y0, int width, int height)
{
  RectArea ra = {0, 0, 0, 0};
  int x1, y1;
  int pixel_size = 1;

  if(ro->state & ROS_REMAP_IGNORE) return ra;
  if(ro->remap_func == NULL) return ra;

  if(x0 < 0) width -= x0, x0 = 0;
  if(y0 < 0) height -= y0, y0 = 0;

  if(x0 >= ro->dst_width || y0 >= ro->dst_height) return ra;
  if(width <= 0 || height <= 0) return ra;

  x1 = x0 + width;
  y1 = y0 + height;

  if(x1 > ro->dst_width) x1 = ro->dst_width, width = x1 - x0;
  if(y1 > ro->dst_height) y1 = ro->dst_height, height = y1 - y0;

  ra.x = x0;
  ra.y = y0;
  ra.width = width;
  ra.height = height;

  if(!(ra.width && ra.height)) return ra;

  switch(ro->dst_mode) {
    case MODE_TRUE_15:
    case MODE_TRUE_16: pixel_size = 2; break;
    case MODE_TRUE_24: pixel_size = 3; break;
    case MODE_TRUE_32: pixel_size = 4; break;
    case MODE_TRUE_8:
    case MODE_PSEUDO_8:
    default: pixel_size = 1;
  }

  if(ro->remap_func_flags & RFF_REMAP_RECT) {
    ro->src_x0 = bre_s(x0, ro->src_width, ro->dst_width);
    ro->src_x1 = bre_s(x1, ro->src_width, ro->dst_width);
    ro->src_y0 = bre_s(y0, ro->src_height, ro->dst_height);
    ro->src_y1 = bre_s(y1, ro->src_height, ro->dst_height);
    ro->src_offset = ro->src_y0 * ro->src_scan_len + ro->src_x0;
    ro->dst_x0 = x0;
    ro->dst_x1 = x1;
    ro->dst_y0 = y0;
    ro->dst_y1 = y1;
    ro->dst_offset = ro->dst_y0 * ro->dst_scan_len + ro->dst_x0 * pixel_size;
    REMAP_AREA_DEBUG_FUNC(ro);
    ro->remap_func(ro);
  }
  else if(ro->remap_func_flags & RFF_REMAP_LINES) {
    ro->src_x0 = 0;
    ro->src_x1 = ro->src_width;
    ro->src_y0 = bre_s(y0, ro->src_height, ro->dst_height);
    ro->src_y1 = bre_s(y1, ro->src_height, ro->dst_height);
    ro->src_offset = ro->src_y0 * ro->src_scan_len;
    ro->dst_x0 = 0;
    ro->dst_x1 = ro->dst_width;
    ro->dst_y0 = y0;
    ro->dst_y1 = y1;
    ro->dst_offset = ro->dst_y0 * ro->dst_scan_len;
    REMAP_AREA_DEBUG_FUNC(ro);
    ro->remap_func(ro);
  }
  else {
    ro->src_offset = ro->dst_offset = 0;
    ro->src_x0 = ro->dst_x0 = ro->src_y0 = ro->dst_y0 = 0;
    ro->src_x1 = ro->src_width;
    ro->dst_x1 = ro->dst_width;
    ro->src_y1 = ro->src_height;
    ro->dst_y1 = ro->dst_height;
    ra.x = ra.y = 0;
    ra.width = ro->dst_width;
    ra.height = ro->dst_height;
    REMAP_AREA_DEBUG_FUNC(ro);
    ro->remap_func(ro);
  }

  return ra;
}


/*
 * for CGA/Hercules-like modes
 */
static RectArea remap_mem_2(RemapObject *ro, int offset, int len)
{
  RectArea ra = {0, 0, 0, 0};
  int i1, i2, j1, j2;

  if(ro->state & ROS_REMAP_IGNORE) return ra;
  if(ro->remap_func == NULL) return ra;

#ifdef REMAP_AREA_DEBUG
  fprintf(rdm, "remap_mem: ofs = %d, len = %d\n", offset, len);
  fprintf(rdm,
    "  src: base = 0x%x, width = %d, height = %d, scan_len = %d\n",
    (unsigned) ro->src_image, ro->src_width, ro->src_height, ro->src_scan_len
  );
  fprintf(rdm,
    "  dst: base = 0x%x, width = %d, height = %d, scan_len = %d\n",
    (unsigned) ro->dst_image, ro->dst_width, ro->dst_height, ro->dst_scan_len
  );
#endif

  if(offset < 0) len += offset, offset = 0;
  if(len <= 0) return ra;

  i1 = offset / ro->src_scan_len;
  i2 = offset % ro->src_scan_len;
  j1 = (offset + len) / ro->src_scan_len;
  j2 = (offset + len) % ro->src_scan_len;

  if(ro->src_mode == MODE_HERC) {
    i1 <<= 2;
    j1 <<= 2; j1 += 3;
  }
  else {	/* CGA */
    i1 <<= 1;
    j1 <<= 1; j1++;
  }

  /* make sure it's all visible */
  if(i2 >= ro->src_width) i1++, i2 = 0, offset = i1 * ro->src_scan_len;
  if(i1 >= ro->src_height || i1 > j1) return ra;
  if(j2 >= ro->src_width) j1++, j2 = 0;
  if(j1 >= ro->src_height) j1 = ro->src_height, j2 = 0;

  ra.width = ro->dst_width;

  if(
    (ro->remap_func_flags & RFF_REMAP_RECT) ||
    (ro->remap_func_flags & RFF_REMAP_LINES)
  ) {
    if(ro->src_mode == MODE_HERC) {
      ro->src_offset = (i1 >> 2) * ro->src_scan_len + (i1 & 3) * 0x2000;
    }
    else {	/* CGA */
      ro->src_offset = (i1 >> 1) * ro->src_scan_len + (i1 & 1 ? 0x2000 : 0);
    }
    ro->src_x0 = ro->dst_x0 = 0;
    ro->src_x1 = ro->src_width; ro->dst_x1 = ro->dst_width;
    ro->src_y0 = i1;
    ro->src_y1 = j1;
    if(j2) ro->src_y1++;
    ro->dst_y0 = bre_d_0(ro->src_y0, ro->src_height, ro->dst_height);
    ro->dst_y1 = bre_d_0(ro->src_y1, ro->src_height, ro->dst_height);
    ro->dst_offset = ro->dst_y0 * ro->dst_scan_len;
    ra.y = ro->dst_y0;
    ra.height = ro->dst_y1 - ro->dst_y0;
    REMAP_AREA_DEBUG_FUNC(ro);
    if(ro->dst_y0 != ro->dst_y1) {
      ro->remap_func(ro);
    }
  }
  else {
    ro->src_offset = ro->dst_offset = 0;
    ro->src_x0 = ro->dst_x0 = ro->src_y0 = ro->dst_y0 = 0;
    ro->src_x1 = ro->src_width;
    ro->dst_x1 = ro->dst_width;
    ro->src_y1 = ro->src_height;
    ro->dst_y1 = ro->dst_height;
    ra.height = ro->dst_height;
    REMAP_AREA_DEBUG_FUNC(ro);
    ro->remap_func(ro);
  }

  return ra;
}


/*
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 *
 *       some functions for managing the list of remap functions
 *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 */

/*
 * Search for the first suitable remap function.
 */
static RemapFuncDesc *find_remap_func(unsigned flags, int src_mode, int dst_mode, RemapFuncDesc *rfd)
{
  while(rfd != NULL) {
    if((rfd->src_mode & src_mode) &&
       (rfd->dst_mode & dst_mode) &&
       (rfd->flags & (flags | RFF_BILIN_FILT | RFF_LIN_FILT)) == flags
    ) break;
    rfd = rfd->next;
  }
  return rfd;
}


/*
 * Searches for the best remap function available.
 * Prefers optimized ones over generic implementations and
 * functions that remap rectangular areas over those that
 * can remap only complete lines.
 */
static RemapFuncDesc *find_best_remap_func(unsigned flags, int src_mode, int dst_mode, RemapFuncDesc *rfd)
{
  RemapFuncDesc *rfd1 = NULL;
  #define REMAB_COMBS 4
  unsigned f_list[6 * REMAB_COMBS];
  int features = 6;
  int i;

  flags &= (RFF_LIN_FILT | RFF_BILIN_FILT | RFF_SCALE_ALL | RFF_SCALE_1 | RFF_SCALE_2);

  f_list[0] = flags | RFF_OPT_PENTIUM | RFF_REMAP_RECT;
  f_list[1] = flags | RFF_OPT_PENTIUM | RFF_REMAP_LINES;
  f_list[2] = flags | RFF_REMAP_RECT;
  f_list[3] = flags | RFF_REMAP_LINES;
  f_list[4] = flags | RFF_OPT_PENTIUM;
  f_list[5] = flags;

  for(i = 0; i < 6; i++) {
    f_list[features     + i] = (f_list[i] & ~RFF_LIN_FILT) | RFF_BILIN_FILT;
    f_list[features * 2 + i] = (f_list[i] & ~RFF_BILIN_FILT) | RFF_LIN_FILT;
    f_list[features * 3 + i] =  f_list[i] & ~(RFF_LIN_FILT | RFF_BILIN_FILT);
  }

  features *= REMAB_COMBS;

  for(i = 0; i < features; i++) {
    if((rfd1 = find_remap_func(f_list[i], src_mode, dst_mode, rfd)) != NULL) break;
  }

  return rfd1;
}


/*
 * Looks up all remap functions that are (possibly) needed
 * for a particular mode. Does _not_ set ro->remap_func!
 */
static void install_remap_funcs(RemapObject *ro, int remap_features)
{
  if (remap_features & RFF_BILIN_FILT)
    remap_features &= ~RFF_LIN_FILT;
  ro->func_all = find_best_remap_func(remap_features | RFF_SCALE_ALL, ro->src_mode, ro->dst_mode, remap_list);
  ro->func_1   = find_best_remap_func(remap_features | RFF_SCALE_1  , ro->src_mode, ro->dst_mode, remap_list);
  ro->func_2   = find_best_remap_func(remap_features | RFF_SCALE_2  , ro->src_mode, ro->dst_mode, remap_list);

  if (ro->func_all)
    ro->state |= ROS_SCALE_ALL;
  /* accept partial scalers only if filtering matches or no full scaler */
  if (ro->func_1 && ((((ro->func_1->flags ^ remap_features) &
      (RFF_BILIN_FILT | RFF_LIN_FILT)) == 0)
      || !ro->func_all))
    ro->state |= ROS_SCALE_1;
  if (ro->func_2 && ((((ro->func_2->flags ^ remap_features) &
      (RFF_BILIN_FILT | RFF_LIN_FILT)) == 0)
      || !ro->func_all))
    ro->state |= ROS_SCALE_2;
  if (!ro->state)
    error("remap function not found for mode %i\n", ro->src_mode);

  ro->supported_src_modes = find_supported_modes(ro->dst_mode);
}

#if 0
static int _find_supported_modes(unsigned dst_mode)
{
  int modes = 0;
  RemapFuncDesc *rfd;
  if(!base_init) {
    do_base_init();
    base_init = 1;
  }
  rfd = remap_list;
  while(rfd != NULL) {
    if(rfd->dst_mode & dst_mode) modes |= rfd->src_mode;
    rfd = rfd->next;
  }
  return modes;
}
#endif

/*
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 *
 *                     some not so optimized remap functions
 *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 */

void gen_1to8_all(RemapObject *);
void gen_1to8p_all(RemapObject *);
void gen_1to16_all(RemapObject *);
void gen_1to24_all(RemapObject *);
void gen_1to32_all(RemapObject *);

void gen_2to8_all(RemapObject *);
void gen_2to8p_all(RemapObject *);
void gen_2to16_all(RemapObject *);
void gen_2to24_all(RemapObject *);
void gen_2to32_all(RemapObject *);

void gen_c2to8_all(RemapObject *);
void gen_c2to8p_all(RemapObject *);
void gen_c2to16_all(RemapObject *);
void gen_c2to24_all(RemapObject *);
void gen_c2to32_all(RemapObject *);

void gen_4to8_all(RemapObject *);
void gen_4to8p_all(RemapObject *);
void gen_4to16_all(RemapObject *);
void gen_4to24_all(RemapObject *);
void gen_4to32_all(RemapObject *);

void gen_8to8_all(RemapObject *);
void gen_8to8_1(RemapObject *);
void gen_8to8p_all(RemapObject *);
void gen_8to8p_1(RemapObject *);
void gen_8to16_all(RemapObject *);
void gen_8to16_lin(RemapObject *);
void gen_8to16_bilin(RemapObject *);
void gen_8to24_all(RemapObject *);
void gen_8to32_all(RemapObject *);
void gen_8to32_1(RemapObject *);
void gen_8to32_lin(RemapObject *);
void gen_8to32_bilin(RemapObject *);

//void gen_15to16_all(RemapObject *);
//void gen_15to24_all(RemapObject *);
void gen_15to32_all(RemapObject *);
void gen_15to32_1(RemapObject *);

//void gen_16to16_all(RemapObject *);
void gen_16to16_1(RemapObject *);
//void gen_16to24_all(RemapObject *);
void gen_16to32_all(RemapObject *);
void gen_16to32_1(RemapObject *);

//void gen_24to24_all(RemapObject *);
void gen_24to24_1(RemapObject *);
void gen_24to32_all(RemapObject *);
void gen_24to32_1(RemapObject *);

void gen_32to32_all(RemapObject *);
void gen_32to32_1(RemapObject *);


static RemapFuncDesc remap_gen_list[] = {

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_VGA_1 | MODE_CGA_1 | MODE_HERC,
    MODE_TRUE_8,
    gen_1to8_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_VGA_1 | MODE_CGA_1 | MODE_HERC,
    MODE_PSEUDO_8,
    gen_1to8p_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_VGA_1 | MODE_CGA_1 | MODE_HERC,
    MODE_TRUE_15 | MODE_TRUE_16,
    gen_1to16_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_VGA_1 | MODE_CGA_1 | MODE_HERC,
    MODE_TRUE_24,
    gen_1to24_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_VGA_1 | MODE_CGA_1 | MODE_HERC,
    MODE_TRUE_32,
    gen_1to32_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_VGA_2,
    MODE_TRUE_8,
    gen_2to8_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_VGA_2,
    MODE_PSEUDO_8,
    gen_2to8p_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_VGA_2,
    MODE_TRUE_15 | MODE_TRUE_16,
    gen_2to16_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_VGA_2,
    MODE_TRUE_24,
    gen_2to24_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_VGA_2,
    MODE_TRUE_32,
    gen_2to32_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_CGA_2,
    MODE_TRUE_8,
    gen_c2to8_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_CGA_2,
    MODE_PSEUDO_8,
    gen_c2to8p_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_CGA_2,
    MODE_TRUE_15 | MODE_TRUE_16,
    gen_c2to16_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_CGA_2,
    MODE_TRUE_24,
    gen_c2to24_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_CGA_2,
    MODE_TRUE_32,
    gen_c2to32_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_VGA_4,
    MODE_TRUE_8,
    gen_4to8_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_VGA_4,
    MODE_PSEUDO_8,
    gen_4to8p_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_VGA_4,
    MODE_TRUE_15 | MODE_TRUE_16,
    gen_4to16_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_VGA_4,
    MODE_TRUE_24,
    gen_4to24_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_VGA_4,
    MODE_TRUE_32,
    gen_4to32_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_VGA_X | MODE_PSEUDO_8,
    MODE_TRUE_8,
    gen_8to8_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_1 | RFF_REMAP_RECT,
    MODE_PSEUDO_8,
    MODE_TRUE_8,
    gen_8to8_1,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_VGA_X | MODE_PSEUDO_8,
    MODE_PSEUDO_8,
    gen_8to8p_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_1 | RFF_REMAP_LINES,
    MODE_PSEUDO_8,
    MODE_PSEUDO_8,
    gen_8to8p_1,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_VGA_X | MODE_PSEUDO_8,
    MODE_TRUE_15 | MODE_TRUE_16,
    gen_8to16_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES | RFF_LIN_FILT,
    MODE_PSEUDO_8,
    MODE_TRUE_15 | MODE_TRUE_16,
    gen_8to16_lin,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES | RFF_BILIN_FILT,
    MODE_PSEUDO_8,
    MODE_TRUE_15 | MODE_TRUE_16,
    gen_8to16_bilin,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_VGA_X | MODE_PSEUDO_8,
    MODE_TRUE_24,
    gen_8to24_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_VGA_X | MODE_PSEUDO_8,
    MODE_TRUE_32,
    gen_8to32_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_1 | RFF_REMAP_RECT,
    MODE_PSEUDO_8,
    MODE_TRUE_32,
    gen_8to32_1,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES | RFF_LIN_FILT,
    MODE_PSEUDO_8,
    MODE_TRUE_32,
    gen_8to32_lin,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES | RFF_BILIN_FILT,
    MODE_PSEUDO_8,
    MODE_TRUE_32,
    gen_8to32_bilin,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_TRUE_15,
    MODE_TRUE_32,
    gen_15to32_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_1 | RFF_REMAP_LINES,
    MODE_TRUE_15,
    MODE_TRUE_32,
    gen_15to32_1,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_1 | RFF_REMAP_LINES,
    MODE_TRUE_15,
    MODE_TRUE_15,
    gen_16to16_1,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_1 | RFF_REMAP_LINES,
    MODE_TRUE_16,
    MODE_TRUE_16,
    gen_16to16_1,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_TRUE_16,
    MODE_TRUE_32,
    gen_16to32_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_1 | RFF_REMAP_LINES,
    MODE_TRUE_16,
    MODE_TRUE_32,
    gen_16to32_1,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_1 | RFF_REMAP_LINES,
    MODE_TRUE_24,
    MODE_TRUE_24,
    gen_24to24_1,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_TRUE_24,
    MODE_TRUE_32,
    gen_24to32_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_1 | RFF_REMAP_LINES,
    MODE_TRUE_24,
    MODE_TRUE_32,
    gen_24to32_1,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_ALL | RFF_REMAP_LINES,
    MODE_TRUE_32,
    MODE_TRUE_32,
    gen_32to32_all,
    NULL
  ),

  REMAP_DESC(
    RFF_SCALE_1 | RFF_REMAP_LINES,
    MODE_TRUE_32,
    MODE_TRUE_32,
    gen_32to32_1,
    NULL
  ),

};

/*
 * returns chained list of modes
 */
RemapFuncDesc *remap_gen(void)
{
  int i;

  for(i = 0; i < sizeof(remap_gen_list) / sizeof(*remap_gen_list) - 1; i++) {
    remap_gen_list[i].next = remap_gen_list + i + 1;
  }

  return remap_gen_list;
}


/*
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 *
 * now the implementation
 *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 */

/*
 * 1 bit pseudo color --> 8 bit true color (shared color map)
 * supports arbitrary scaling
 *
 * -- very basic and slow --
 */
void gen_1to8_all(RemapObject *ro)
{
  int k;
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len;
  int *bre_x;
  int *bre_y = ro->bre_y;
  const unsigned char *src, *src0;
  unsigned char *dst;
  unsigned char *lut = (unsigned char *)ro->true_color_lut;
  unsigned char c0;
  int i;

  src0 = ro->src_image + ro->src_start;
  dst = ro->dst_image + ro->dst_start + ro->dst_offset;
  d_x_len = ro->dst_width;

  for (d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    k = (d_y & 1) << 1;
    for (s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len;) {
      i = s_x >> 3;
      c0 = src[i];
      i = (s_x & 7) ^ 7;
      c0 >>= i;
      c0 &= 1;
      dst[d_x++] = lut[4 * c0 + (k ^= 1)];
      s_x += *(bre_x++);
    }
  }
}

/*
 * 1 bit pseudo color --> 8 bit pseudo color (private color map)
 * supports arbitrary scaling
 *
 * -- very basic and slow --
 */
void gen_1to8p_all(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len;
  int *bre_x;
  int *bre_y = ro->bre_y;
  unsigned char c0;
  int i;

  const unsigned char *src, *src0;
  unsigned char *dst;

  src0 = ro->src_image + ro->src_start;
  dst = ro->dst_image + ro->dst_start + ro->dst_offset;
  d_x_len = ro->dst_width;

  for (d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    for (s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len;) {
      i = s_x >> 3;
      c0 = src[i];
      i = (s_x & 7) ^ 7;
      c0 >>= i;
      c0 &= 1;
      dst[d_x++] = c0;
      s_x += *(bre_x++);
    }
  }
}

/*
 * 1 bit pseudo color --> 15/16 bit true color
 * supports arbitrary scaling
 *
 * -- very basic and slow --
 */
void gen_1to16_all(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len >> 1;
  int *bre_x;
  int *bre_y = ro->bre_y;
  unsigned char c0;

  const unsigned char *src, *src0;
  unsigned short *dst;

  src0 = ro->src_image + ro->src_start;
  dst = (unsigned short *)(ro->dst_image + ro->dst_start + ro->dst_offset);
  d_x_len = ro->dst_width;

  for (d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    for (s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len;) {
      c0 = (src[s_x >> 3] >> ((s_x & 7) ^ 7)) & 1;
      dst[d_x++] = ro->true_color_lut[c0];
      s_x += *(bre_x++);
    }
  }
}

/*
 * 1 bit pseudo color --> 24 bit true color
 * supports arbitrary scaling
 *
 * -- very basic and slow --
 */
void gen_1to24_all(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len;
  int *bre_x;
  int *bre_y = ro->bre_y;
  unsigned char c0;

  const unsigned char *src, *src0;
  unsigned char *dst;
  unsigned color;

  src0 = ro->src_image + ro->src_start;
  dst = (ro->dst_image + ro->dst_start + ro->dst_offset);
  d_x_len = ro->dst_width * 3;

  for (d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    for (s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len;) {
      c0 = (src[s_x >> 3] >> ((s_x & 7) ^ 7)) & 1;
      color = ro->true_color_lut[c0];
      dst[d_x++] = color & 0xFF;
      dst[d_x++] = (color >> 8) & 0xFF;
      dst[d_x++] = (color >> 16) & 0xFF;
      s_x += *(bre_x++);
    }
  }
}

/*
 * 1 bit pseudo color --> 32 bit true color
 * supports arbitrary scaling
 *
 * -- very basic and slow --
 */
void gen_1to32_all(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len >> 2;
  int *bre_x;
  int *bre_y = ro->bre_y;
  unsigned char c0;

  const unsigned char *src, *src0;
  unsigned *dst;

  src0 = ro->src_image + ro->src_start;
  dst = (unsigned *)(ro->dst_image + ro->dst_start + ro->dst_offset);
  d_x_len = ro->dst_width;

  for (d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    for (s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len;) {
      c0 = (src[s_x >> 3] >> ((s_x & 7) ^ 7)) & 1;
      dst[d_x++] = ro->true_color_lut[c0];
      s_x += *(bre_x++);
    }
  }
}

/*
 * 2 bit VGA pseudo color --> 8 bit true color (shared color map)
 * supports arbitrary scaling
 *
 * -- very basic and slow --
 */
void gen_2to8_all(RemapObject *ro)
{
  int k;
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len;
  int *bre_x;
  int *bre_y = ro->bre_y;
  const unsigned char *src, *src0;
  unsigned char *dst;
  unsigned char *lut = (unsigned char *)ro->true_color_lut;
  unsigned char c0, c1;
  int i;

  src0 = ro->src_image + ro->src_start;
  dst = ro->dst_image + ro->dst_start + ro->dst_offset;
  d_x_len = ro->dst_width;

  for (d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    k = (d_y & 1) << 1;
    for (s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len;) {
      i = s_x >> 3;
      c0 = src[i];
      c1 = src[i + 0x20000];
      i = (s_x & 7) ^ 7;
      c0 >>= i;
      c1 >>= i;
      c0 &= 1;
      c1 &= 1;
      c0 |= c1 << 1;
      dst[d_x++] = lut[4 * c0 + (k ^= 1)];
      s_x += *(bre_x++);
    }
  }
}

/*
 * 2 bit VGA pseudo color --> 8 bit pseudo color (private color map)
 * supports arbitrary scaling
 *
 * -- very basic and slow --
 */
void gen_2to8p_all(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len;
  int *bre_x;
  int *bre_y = ro->bre_y;
  unsigned char c0, c1;
  int i;

  const unsigned char *src, *src0;
  unsigned char *dst;

  src0 = ro->src_image + ro->src_start;
  dst = ro->dst_image + ro->dst_start + ro->dst_offset;
  d_x_len = ro->dst_width;

  for (d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    for (s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len;) {
      i = s_x >> 3;
      c0 = src[i];
      c1 = src[i + 0x20000];
      i = (s_x & 7) ^ 7;
      c0 >>= i;
      c1 >>= i;
      c0 &= 1;
      c1 &= 1;
      c0 |= c1 << 1;
      dst[d_x++] = c0;
      s_x += *(bre_x++);
    }
  }
}

/*
 * 2 bit VGA pseudo color --> 15/16 bit true color
 * supports arbitrary scaling
 *
 * -- very basic and slow --
 */
void gen_2to16_all(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len >> 1;
  int *bre_x;
  int *bre_y = ro->bre_y;
  unsigned char c0, c1;
  int i;

  const unsigned char *src, *src0;
  unsigned short *dst;

  src0 = ro->src_image + ro->src_start;
  dst = (unsigned short *)(ro->dst_image + ro->dst_start + ro->dst_offset);
  d_x_len = ro->dst_width;

  for (d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    for (s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len;) {
      i = s_x >> 3;
      c0 = src[i];
      c1 = src[i + 0x20000];
      i = (s_x & 7) ^ 7;
      c0 >>= i;
      c1 >>= i;
      c0 &= 1;
      c1 &= 1;
      c0 |= (c1 << 1);
      dst[d_x++] = ro->true_color_lut[c0];
      s_x += *(bre_x++);
    }
  }
}

/*
 * 2 bit VGA pseudo color --> 24 bit true color
 * supports arbitrary scaling
 *
 * -- very basic and slow --
 */
void gen_2to24_all(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len;
  int *bre_x;
  int *bre_y = ro->bre_y;
  unsigned char c0, c1;
  int i;

  const unsigned char *src, *src0;
  unsigned char *dst;
  unsigned color;

  src0 = ro->src_image + ro->src_start;
  dst = (ro->dst_image + ro->dst_start + ro->dst_offset);
  d_x_len = ro->dst_width * 3;

  for (d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    for (s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len;) {
      i = s_x >> 3;
      c0 = src[i];
      c1 = src[i + 0x20000];
      i = (s_x & 7) ^ 7;
      c0 >>= i;
      c1 >>= i;
      c0 &= 1;
      c1 &= 1;
      c0 |= c1 << 1;
      color = ro->true_color_lut[c0];
      dst[d_x++] = color & 0xFF;
      dst[d_x++] = (color >> 8) & 0xFF;
      dst[d_x++] = (color >> 16) & 0xFF;
      s_x += *(bre_x++);
    }
  }
}

/*
 * 2 bit VGA pseudo color --> 32 bit true color
 * supports arbitrary scaling
 *
 * -- very basic and slow --
 */
void gen_2to32_all(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len >> 2;
  int *bre_x;
  int *bre_y = ro->bre_y;
  unsigned char c0, c1;
  int i;

  const unsigned char *src, *src0;
  unsigned *dst;

  src0 = ro->src_image + ro->src_start;
  dst = (unsigned *)(ro->dst_image + ro->dst_start + ro->dst_offset);
  d_x_len = ro->dst_width;

  for (d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    for (s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len;) {
      i = s_x >> 3;
      c0 = src[i];
      c1 = src[i + 0x20000];
      i = (s_x & 7) ^ 7;
      c0 >>= i;
      c1 >>= i;
      c0 &= 1;
      c1 &= 1;
      c0 |= c1 << 1;
      dst[d_x++] = ro->true_color_lut[c0];
      s_x += *(bre_x++);
    }
  }
}

/*
 * 2 bit CGA pseudo color --> 8 bit true color (shared color map)
 * supports arbitrary scaling
 *
 * -- very basic and slow --
 */
void gen_c2to8_all(RemapObject *ro)
{
  int k;
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len;
  int *bre_x;
  int *bre_y = ro->bre_y;
  const unsigned char *src, *src0;
  unsigned char *dst;
  unsigned char *lut = (unsigned char *)ro->true_color_lut;
  unsigned char c0;
  int i;

  src0 = ro->src_image + ro->src_start;
  dst = ro->dst_image + ro->dst_start + ro->dst_offset;
  d_x_len = ro->dst_width;

  for (d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    k = (d_y & 1) << 1;
    for (s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len;) {
      i = s_x >> 2;
      c0 = src[i];
      i = 2 * ((s_x & 3) ^ 3);
      c0 >>= i;
      c0 &= 3;
      dst[d_x++] = lut[4 * c0 + (k ^= 1)];
      s_x += *(bre_x++);
    }
  }
}

/*
 * 2 bit CGA pseudo color --> 8 bit pseudo color (private color map)
 * supports arbitrary scaling
 *
 * -- very basic and slow --
 */
void gen_c2to8p_all(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len;
  int *bre_x;
  int *bre_y = ro->bre_y;
  unsigned char c0;
  int i;

  const unsigned char *src, *src0;
  unsigned char *dst;

  src0 = ro->src_image + ro->src_start;
  dst = ro->dst_image + ro->dst_start + ro->dst_offset;
  d_x_len = ro->dst_width;

  for (d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    for (s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len;) {
      i = s_x >> 2;
      c0 = src[i];
      i = 2 * ((s_x & 3) ^ 3);
      c0 >>= i;
      c0 &= 3;
      dst[d_x++] = c0;
      s_x += *(bre_x++);
    }
  }
}

/*
 * 2 bit CGA pseudo color --> 15/16 bit true color
 * supports arbitrary scaling
 *
 * -- very basic and slow --
 */
void gen_c2to16_all(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len >> 1;
  int *bre_x;
  int *bre_y = ro->bre_y;
  unsigned char c0;

  const unsigned char *src, *src0;
  unsigned short *dst;

  src0 = ro->src_image + ro->src_start;
  dst = (unsigned short *)(ro->dst_image + ro->dst_start + ro->dst_offset);
  d_x_len = ro->dst_width;

  for (d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    for (s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len;) {
      c0 = (src[s_x >> 2] >> (2 * ((s_x & 3) ^ 3))) & 3;
      dst[d_x++] = ro->true_color_lut[c0];
      s_x += *(bre_x++);
    }
  }
}

/*
 * 2 bit CGA pseudo color --> 24 bit true color
 * supports arbitrary scaling
 *
 * -- very basic and slow --
 */
void gen_c2to24_all(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len;
  int *bre_x;
  int *bre_y = ro->bre_y;
  unsigned char c0;

  const unsigned char *src, *src0;
  unsigned char *dst;
  unsigned color;

  src0 = ro->src_image + ro->src_start;
  dst = (ro->dst_image + ro->dst_start + ro->dst_offset);
  d_x_len = ro->dst_width * 3;

  for (d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    for (s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len;) {
      c0 = (src[s_x >> 2] >> (2 * ((s_x & 3) ^ 3))) & 3;
      color = ro->true_color_lut[c0];
      dst[d_x++] = color & 0xFF;
      dst[d_x++] = (color >> 8) & 0xFF;
      dst[d_x++] = (color >> 16) & 0xFF;
      s_x += *(bre_x++);
    }
  }
}

/*
 * 2 bit CGA pseudo color --> 32 bit true color
 * supports arbitrary scaling
 *
 * -- very basic and slow --
 */
void gen_c2to32_all(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len >> 2;
  int *bre_x;
  int *bre_y = ro->bre_y;
  unsigned char c0;

  const unsigned char *src, *src0;
  unsigned *dst;

  src0 = ro->src_image + ro->src_start;
  dst = (unsigned *)(ro->dst_image + ro->dst_start + ro->dst_offset);
  d_x_len = ro->dst_width;

  for (d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    for (s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len;) {
      c0 = (src[s_x >> 2] >> (2 * ((s_x & 3) ^ 3))) & 3;
      dst[d_x++] = ro->true_color_lut[c0];
      s_x += *(bre_x++);
    }
  }
}

/*
 * 4 bit pseudo color --> 8 bit true color (shared color map)
 * supports arbitrary scaling
 *
 */
void gen_4to8_all(RemapObject *ro)
{
  int k;
  int d_x_len, s_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len;
  int *bre_x;
  int *bre_y = ro->bre_y;

  const unsigned char *src, *src0, *src_last;
  unsigned char *clut = (unsigned char*) ro->true_color_lut, *dst, *src1;
  unsigned *dst1, *lut;

  src0 = ro->src_image + ro->src_start;
  dst = ro->dst_image + ro->dst_start + ro->dst_offset;
  d_x_len = ro->dst_width;
  s_x_len = ro->src_width >> 3;
  src1 = ro->src_tmp_line;
  dst1 = (unsigned *) src1;
  lut = ro->bit_lut;
  src_last = NULL;

  for(d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    k = (d_y & 1) << 1;
    if(src != src_last) {
      src_last = src;
      for(s_x = d_x = 0; s_x < s_x_len; s_x++, d_x += 2) {
        dst1[d_x    ]  = lut[2 * src[s_x          ]            ] |
                         lut[2 * src[s_x + 0x10000]     + 0x200] |
                         lut[2 * src[s_x + 0x20000]     + 0x400] |
                         lut[2 * src[s_x + 0x30000]     + 0x600];
        dst1[d_x + 1]  = lut[2 * src[s_x          ] + 1        ] |
                         lut[2 * src[s_x + 0x10000] + 1 + 0x200] |
                         lut[2 * src[s_x + 0x20000] + 1 + 0x400] |
                         lut[2 * src[s_x + 0x30000] + 1 + 0x600];
      }
    }
    for(s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len; ) {
      dst[d_x++] = clut[4 * src1[s_x] + (k ^= 1)];
      s_x += *(bre_x++);
    }
  }
}

/*
 * 4 bit pseudo color --> 8 bit pseudo color (private color map)
 * supports arbitrary scaling
 *
 */
void gen_4to8p_all(RemapObject *ro)
{
  int d_x_len, s_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len;
  int *bre_x;
  int *bre_y = ro->bre_y;

  const unsigned char *src, *src0, *src_last;
  unsigned char *dst, *src1;
  unsigned *dst1, *lut;

  src0 = ro->src_image + ro->src_start;
  dst = ro->dst_image + ro->dst_start + ro->dst_offset;
  d_x_len = ro->dst_width;
  s_x_len = ro->src_width >> 3;
  src1 = ro->src_tmp_line;
  dst1 = (unsigned *) src1;
  lut = ro->bit_lut;
  src_last = NULL;

  for(d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    if(src != src_last) {
      src_last = src;
      for(s_x = d_x = 0; s_x < s_x_len; s_x++, d_x += 2) {
        dst1[d_x    ]  = lut[2 * src[s_x          ]            ] |
                         lut[2 * src[s_x + 0x10000]     + 0x200] |
                         lut[2 * src[s_x + 0x20000]     + 0x400] |
                         lut[2 * src[s_x + 0x30000]     + 0x600];
        dst1[d_x + 1]  = lut[2 * src[s_x          ] + 1        ] |
                         lut[2 * src[s_x + 0x10000] + 1 + 0x200] |
                         lut[2 * src[s_x + 0x20000] + 1 + 0x400] |
                         lut[2 * src[s_x + 0x30000] + 1 + 0x600];
      }
    }
    for(s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len; ) {
      dst[d_x++] = src1[s_x];
      s_x += *(bre_x++);
    }
  }
}

/*
 * 4 bit pseudo color --> 15/16 bit true color
 * supports arbitrary scaling
 *
 */
void gen_4to16_all(RemapObject *ro)
{
  int d_x_len, s_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len >> 1;
  int *bre_x;
  int *bre_y = ro->bre_y;

  const unsigned char *src, *src0, *src_last;
  unsigned char *src1;
  unsigned short *dst;
  unsigned *dst1, *lut;

  src0 = ro->src_image + ro->src_start;
  dst = (unsigned short *) (ro->dst_image + ro->dst_start + ro->dst_offset);
  d_x_len = ro->dst_width;
  s_x_len = ro->src_width >> 3;
  src1 = ro->src_tmp_line;
  dst1 = (unsigned *) src1;
  lut = ro->bit_lut;
  src_last = NULL;

  for(d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    if(src != src_last) {
      src_last = src;
      for(s_x = d_x = 0; s_x < s_x_len; s_x++, d_x += 2) {
        dst1[d_x    ]  = lut[2 * src[s_x          ]            ] |
                         lut[2 * src[s_x + 0x10000]     + 0x200] |
                         lut[2 * src[s_x + 0x20000]     + 0x400] |
                         lut[2 * src[s_x + 0x30000]     + 0x600];
        dst1[d_x + 1]  = lut[2 * src[s_x          ] + 1        ] |
                         lut[2 * src[s_x + 0x10000] + 1 + 0x200] |
                         lut[2 * src[s_x + 0x20000] + 1 + 0x400] |
                         lut[2 * src[s_x + 0x30000] + 1 + 0x600];
      }
    }
    for(s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len; ) {
      dst[d_x++] = ro->true_color_lut[src1[s_x]];
      s_x += *(bre_x++);
    }
  }
}

/*
 * 4 bit pseudo color --> 24 bit true color
 * supports arbitrary scaling
 *
 */
void gen_4to24_all(RemapObject *ro)
{
  int d_x_len, s_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len;
  int *bre_x;
  int *bre_y = ro->bre_y;

  unsigned *dst1, *lut;
  const unsigned char *src, *src0, *src_last;
  unsigned char *dst, *src1;
  unsigned color;

  src0 = ro->src_image + ro->src_start;
  dst = (ro->dst_image + ro->dst_start + ro->dst_offset);
  d_x_len = ro->dst_width *3;
  s_x_len = ro->src_width >> 3;
  src1 = ro->src_tmp_line;
  dst1 = (unsigned *) src1;
  lut = ro->bit_lut;
  src_last = NULL;

  for(d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    if(src != src_last) {
      src_last = src;
      for(s_x = d_x = 0; s_x < s_x_len; s_x++, d_x += 2) {
        dst1[d_x    ]  = lut[2 * src[s_x          ]            ] |
                         lut[2 * src[s_x + 0x10000]     + 0x200] |
                         lut[2 * src[s_x + 0x20000]     + 0x400] |
                         lut[2 * src[s_x + 0x30000]     + 0x600];
        dst1[d_x + 1]  = lut[2 * src[s_x          ] + 1        ] |
                         lut[2 * src[s_x + 0x10000] + 1 + 0x200] |
                         lut[2 * src[s_x + 0x20000] + 1 + 0x400] |
                         lut[2 * src[s_x + 0x30000] + 1 + 0x600];
      }
    }
    for(s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len; ) {
      color = ro->true_color_lut[src1[s_x]];
      dst[d_x++] = color & 0xFF;
      dst[d_x++] = (color >> 8) & 0xFF;
      dst[d_x++] = (color >> 16) & 0xFF;
      s_x += *(bre_x++);
    }
  }
}

/*
 * 4 bit pseudo color --> 32 bit true color
 * supports arbitrary scaling
 *
 */
void gen_4to32_all(RemapObject *ro)
{
  int d_x_len, s_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len >> 2;
  int *bre_x;
  int *bre_y = ro->bre_y;

  unsigned *dst1, *lut;
  const unsigned char *src, *src0, *src_last;
  unsigned char *src1;
  unsigned *dst;

  src0 = ro->src_image + ro->src_start;
  dst = (unsigned *) (ro->dst_image + ro->dst_start + ro->dst_offset);
  d_x_len = ro->dst_width;
  s_x_len = ro->src_width >> 3;
  src1 = ro->src_tmp_line;
  dst1 = (unsigned *) src1;
  lut = ro->bit_lut;
  src_last = NULL;

  for(d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    if(src != src_last) {
      src_last = src;
      for(s_x = d_x = 0; s_x < s_x_len; s_x++, d_x += 2) {
        dst1[d_x    ]  = lut[2 * src[s_x          ]            ] |
                         lut[2 * src[s_x + 0x10000]     + 0x200] |
                         lut[2 * src[s_x + 0x20000]     + 0x400] |
                         lut[2 * src[s_x + 0x30000]     + 0x600];
        dst1[d_x + 1]  = lut[2 * src[s_x          ] + 1        ] |
                         lut[2 * src[s_x + 0x10000] + 1 + 0x200] |
                         lut[2 * src[s_x + 0x20000] + 1 + 0x400] |
                         lut[2 * src[s_x + 0x30000] + 1 + 0x600];
      }
    }
    for(s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len; ) {
      dst[d_x++] = ro->true_color_lut[src1[s_x]];
      s_x += *(bre_x++);
    }
  }
}

/*
 * 8 bit pseudo color --> 8 bit true color (shared color map)
 * supports arbitrary scaling
 */
void gen_8to8_all(RemapObject *ro)
{
  int k;
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len;
  int *bre_x;
  int *bre_y = ro->bre_y;
  const unsigned char *src, *src0;
  unsigned char *dst;
  unsigned char *lut = (unsigned char *)ro->true_color_lut;

  src0 = ro->src_image + ro->src_start;
  dst = ro->dst_image + ro->dst_start + ro->dst_offset;
  d_x_len = ro->dst_width;

  for (d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    k = (d_y & 1) << 1;
    for (s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len;) {
      dst[d_x++] = lut[4 * src[s_x] + (k ^= 1)];
      s_x += *(bre_x++);
    }
  }
}

/*
 * 8 bit pseudo color --> 8 bit true color (shared color map)
 */
void gen_8to8_1(RemapObject *ro)
{
  int i, j, l, k;
  const unsigned char *src;
  unsigned char *dst;
  unsigned char *lut = (unsigned char *)ro->true_color_lut;

  src = ro->src_image + ro->src_start + ro->src_offset;
  dst = ro->dst_image + ro->dst_start + ro->dst_offset;

  l = ro->src_x1 - ro->src_x0;

  for (j = ro->src_y0; j < ro->src_y1; j++) {
    k = (j & 1) << 1;
    for (i = 0; i < l; i++) {
      dst[i] = lut[4 * src[i] + (k ^= 1)];
    }
    dst += ro->dst_scan_len;
    src += ro->src_scan_len;
  }
}

/*
 * 8 bit pseudo color --> 8 bit pseudo color (private color map)
 * supports arbitrary scaling
 */
void gen_8to8p_all(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len;
  int *bre_x;
  int *bre_y = ro->bre_y;

  const unsigned char *src, *src0;
  unsigned char *dst;

  src0 = ro->src_image + ro->src_start;
  dst = ro->dst_image + ro->dst_start + ro->dst_offset;
  d_x_len = ro->dst_width;

  for(d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    for(s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len; ) {
      dst[d_x++] = src[s_x];
      s_x += *(bre_x++);
    }
  }
}

/*
 * 8 bit pseudo color --> 8 bit pseudo color (private color map)
 */
void gen_8to8p_1(RemapObject *ro)
{
  int i;
  const unsigned char *src;
  unsigned char *dst;

  src = ro->src_image + ro->src_start + ro->src_offset;
  dst = ro->dst_image + ro->dst_start + ro->dst_offset;

  for(i = ro->src_y0; i < ro->src_y1; i++) {
    memcpy(dst, src, ro->src_width);
    src += ro->src_scan_len;
    dst += ro->dst_scan_len;
  }
}

/*
 * 8 bit pseudo color --> 15/16 bit true color
 * supports arbitrary scaling
 */
void gen_8to16_all(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len >> 1;
  int *bre_x;
  int *bre_y = ro->bre_y;

  const unsigned char *src, *src0;
  unsigned short *dst;

  src0 = ro->src_image + ro->src_start;
  dst = (unsigned short *) (ro->dst_image + ro->dst_start + ro->dst_offset);
  d_x_len = ro->dst_width;

  for(d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    for(s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len; ) {
      dst[d_x++] = ro->true_color_lut[src[s_x]];
      s_x += *(bre_x++);
    }
  }
}

/*
 * 8 bit pseudo color --> 15/16 bit true color
 * supports arbitrary scaling
 */
void gen_8to16_lin(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len >> 1;
  int *bre_x;
  int *bre_y = ro->bre_y;

  const unsigned char *src, *src0;
  unsigned short *dst;

  src0 = ro->src_image + ro->src_start;
  dst = (unsigned short *) (ro->dst_image + ro->dst_start + ro->dst_offset);
  d_x_len = ro->dst_width;

  for(d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    for(s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len; ) {
      switch(*(bre_x + d_x_len)) {
        case 0:
          dst[d_x++] = ro->true_color_lut[src[s_x]];
          break;
        case 1:
          dst[d_x++] = ro->true_color_lut[src[s_x] + LUT_OFS_67] + ro->true_color_lut[src[s_x + 1] + LUT_OFS_33];
          break;
        case 2:
          dst[d_x++] = ro->true_color_lut[src[s_x] + LUT_OFS_33] + ro->true_color_lut[src[s_x + 1] + LUT_OFS_67];
          break;
        default:
          fprintf(stderr, "***** oops\n");
      }
      s_x += *(bre_x++);
    }
  }
}

/*
 * 8 bit pseudo color --> 15/16 bit true color
 * supports arbitrary scaling
 */
void gen_8to16_bilin(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len >> 1;
  int s_scan_len = ro->src_scan_len;
  int *bre_x;
  int *bre_y = ro->bre_y;

  const unsigned char *src, *src0;
  unsigned short *dst;
  unsigned *lut = ro->true_color_lut;

  src0 = ro->src_image + ro->src_start;
  dst = (unsigned short *) (ro->dst_image + ro->dst_start + ro->dst_offset);
  d_x_len = ro->dst_width;

  for(d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];

    switch(*(bre_y + d_y - 1 + ro->dst_height)) {
      case 0:
        for(s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len; ) {
          switch(*(bre_x + d_x_len)) {
            case 0:
              dst[d_x++] = lut[src[s_x]];
              break;
            case 1:
              dst[d_x++] = lut[src[s_x] + LUT_OFS_67] + lut[src[s_x + 1] + LUT_OFS_33];
              break;
            case 2:
              dst[d_x++] = lut[src[s_x] + LUT_OFS_33] + lut[src[s_x + 1] + LUT_OFS_67];
              break;
            default:
              fprintf(stderr, "***** oops\n");
          }
          s_x += *(bre_x++);
        }
        break;

      case 1:
        for(s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len; ) {
          switch(*(bre_x + d_x_len)) {
            case 0:
              dst[d_x++] = lut[src[s_x             ] + LUT_OFS_67]
                         + lut[src[s_x + s_scan_len] + LUT_OFS_33];
              break;
            case 1:
              dst[d_x++] = lut[src[s_x             ] + LUT_OFS_45] + lut[src[s_x + 1             ] + LUT_OFS_22]
                         + lut[src[s_x + s_scan_len] + LUT_OFS_22] + lut[src[s_x + s_scan_len + 1] + LUT_OFS_11];
              break;
            case 2:
              dst[d_x++] = lut[src[s_x             ] + LUT_OFS_22] + lut[src[s_x + 1             ] + LUT_OFS_45]
                         + lut[src[s_x + s_scan_len] + LUT_OFS_11] + lut[src[s_x + s_scan_len + 1] + LUT_OFS_22];
              break;
            default:
              fprintf(stderr, "***** oops\n");
          }
          s_x += *(bre_x++);
        }
        break;

      case 2:
        for(s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len; ) {
          switch(*(bre_x + d_x_len)) {
            case 0:
              dst[d_x++] = lut[src[s_x             ] + LUT_OFS_33]
                         + lut[src[s_x + s_scan_len] + LUT_OFS_67];
              break;
            case 1:
              dst[d_x++] = lut[src[s_x             ] + LUT_OFS_22] + lut[src[s_x + 1             ] + LUT_OFS_11]
                         + lut[src[s_x + s_scan_len] + LUT_OFS_45] + lut[src[s_x + s_scan_len + 1] + LUT_OFS_22];
              break;
            case 2:
              dst[d_x++] = lut[src[s_x             ] + LUT_OFS_11] + lut[src[s_x + 1             ] + LUT_OFS_22]
                         + lut[src[s_x + s_scan_len] + LUT_OFS_22] + lut[src[s_x + s_scan_len + 1] + LUT_OFS_45];
              break;
            default:
              fprintf(stderr, "***** oops\n");
          }
          s_x += *(bre_x++);
        }
        break;

      default:
        fprintf(stderr, "###### oops\n");
    }

  }
}

/*
 * 8 bit pseudo color --> 24 bit true color
 * supports arbitrary scaling
 */
void gen_8to24_all(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len;
  int *bre_x;
  int *bre_y = ro->bre_y;

  const unsigned char *src, *src0;
  unsigned char *dst;
  unsigned long long color;

  src0 = ro->src_image + ro->src_start;
  dst = (ro->dst_image + ro->dst_start + ro->dst_offset);
  d_x_len = ro->dst_width *3;

  for(d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    for(s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len; ) {
      color = ro->true_color_lut[src[s_x]];
      dst[d_x++] = color & 0xFF;
      dst[d_x++] = (color >> 8) & 0xFF;
      dst[d_x++] = (color >> 16) & 0xFF;
      s_x += *(bre_x++);
    }
  }
}

/*
 * 8 bit pseudo color --> 32 bit true color
 * supports arbitrary scaling
 */
void gen_8to32_all(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len >> 2;
  int *bre_x;
  int *bre_y = ro->bre_y;

  const unsigned char *src, *src0;
  unsigned *dst;

  src0 = ro->src_image + ro->src_start;
  dst = (unsigned *) (ro->dst_image + ro->dst_start + ro->dst_offset);
  d_x_len = ro->dst_width;

  for(d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    for(s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len; ) {
      dst[d_x++] = ro->true_color_lut[src[s_x]];
      s_x += *(bre_x++);
    }
  }
}

/*
 * 8 bit pseudo color --> 32 bit true color
 */
void gen_8to32_1(RemapObject *ro)
{
  int i, j, l;
  const unsigned char *src;
  unsigned *dst;

  src = ro->src_image + ro->src_start + ro->src_offset;
  dst = (unsigned *) (ro->dst_image + ro->dst_start + ro->dst_offset);
  l = (ro->src_x1 - ro->src_x0);

  for(j = ro->src_y0; j < ro->src_y1; j++) {
    for(i = 0; i < l; i++) {
      dst[i] = ro->true_color_lut[src[i]];
    }
    dst += ro->dst_scan_len >> 2;
    src += ro->src_scan_len;
  }
}

/*
 * 8 bit pseudo color --> 32 bit true color
 * supports arbitrary scaling
 */
void gen_8to32_lin(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len >> 2;
  int *bre_x;
  int *bre_y = ro->bre_y;

  const unsigned char *src, *src0;
  unsigned *dst;

  src0 = ro->src_image + ro->src_start;
  dst = (unsigned *) (ro->dst_image + ro->dst_start + ro->dst_offset);
  d_x_len = ro->dst_width;

  for(d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    for(s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len; ) {
      switch(*(bre_x + d_x_len)) {
        case 0:
          dst[d_x++] = ro->true_color_lut[src[s_x]];
          break;
        case 1:
          dst[d_x++] = ro->true_color_lut[src[s_x] + LUT_OFS_67] + ro->true_color_lut[src[s_x + 1] + LUT_OFS_33];
          break;
        case 2:
          dst[d_x++] = ro->true_color_lut[src[s_x] + LUT_OFS_33] + ro->true_color_lut[src[s_x + 1] + LUT_OFS_67];
          break;
        default:
          fprintf(stderr, "***** oops\n");
      }
      s_x += *(bre_x++);
    }
  }
}

/*
 * 8 bit pseudo color --> 32 bit true color
 * supports arbitrary scaling
 */
void gen_8to32_bilin(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len >> 2;
  int s_scan_len = ro->src_scan_len;
  int *bre_x;
  int *bre_y = ro->bre_y;

  const unsigned char *src, *src0;
  unsigned *dst;
  unsigned *lut = ro->true_color_lut;

  src0 = ro->src_image + ro->src_start;
  dst = (unsigned *) (ro->dst_image + ro->dst_start + ro->dst_offset);
  d_x_len = ro->dst_width;

  for(d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];

    switch(*(bre_y + d_y - 1 + ro->dst_height)) {
      case 0:
        for(s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len; ) {
          switch(*(bre_x + d_x_len)) {
            case 0:
              dst[d_x++] = lut[src[s_x]];
              break;
            case 1:
              dst[d_x++] = lut[src[s_x] + LUT_OFS_67] + lut[src[s_x + 1] + LUT_OFS_33];
              break;
            case 2:
              dst[d_x++] = lut[src[s_x] + LUT_OFS_33] + lut[src[s_x + 1] + LUT_OFS_67];
              break;
            default:
              fprintf(stderr, "***** oops\n");
          }
          s_x += *(bre_x++);
        }
        break;

      case 1:
        for(s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len; ) {
          switch(*(bre_x + d_x_len)) {
            case 0:
              dst[d_x++] = lut[src[s_x             ] + LUT_OFS_67]
                         + lut[src[s_x + s_scan_len] + LUT_OFS_33];
              break;
            case 1:
              dst[d_x++] = lut[src[s_x             ] + LUT_OFS_45] + lut[src[s_x + 1             ] + LUT_OFS_22]
                         + lut[src[s_x + s_scan_len] + LUT_OFS_22] + lut[src[s_x + s_scan_len + 1] + LUT_OFS_11];
              break;
            case 2:
              dst[d_x++] = lut[src[s_x             ] + LUT_OFS_22] + lut[src[s_x + 1             ] + LUT_OFS_45]
                         + lut[src[s_x + s_scan_len] + LUT_OFS_11] + lut[src[s_x + s_scan_len + 1] + LUT_OFS_22];
              break;
            default:
              fprintf(stderr, "***** oops\n");
          }
          s_x += *(bre_x++);
        }
        break;

      case 2:
        for(s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len; ) {
          switch(*(bre_x + d_x_len)) {
            case 0:
              dst[d_x++] = lut[src[s_x             ] + LUT_OFS_33]
                         + lut[src[s_x + s_scan_len] + LUT_OFS_67];
              break;
            case 1:
              dst[d_x++] = lut[src[s_x             ] + LUT_OFS_22] + lut[src[s_x + 1             ] + LUT_OFS_11]
                         + lut[src[s_x + s_scan_len] + LUT_OFS_45] + lut[src[s_x + s_scan_len + 1] + LUT_OFS_22];
              break;
            case 2:
              dst[d_x++] = lut[src[s_x             ] + LUT_OFS_11] + lut[src[s_x + 1             ] + LUT_OFS_22]
                         + lut[src[s_x + s_scan_len] + LUT_OFS_22] + lut[src[s_x + s_scan_len + 1] + LUT_OFS_45];
              break;
            default:
              fprintf(stderr, "***** oops\n");
          }
          s_x += *(bre_x++);
        }
        break;

      default:
        fprintf(stderr, "###### oops\n");
    }

  }
}

/*
 * 15 bit true color --> 32 bit true color
 * supports arbitrary scaling
 */
void gen_15to32_all(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len;
  int *bre_x;
  int *bre_y = ro->bre_y;

  const unsigned char *src, *src0;
  unsigned char *dst;
  const unsigned short *src_2;
  unsigned *dst_4;

  src0 = ro->src_image + ro->src_start;
  dst = ro->dst_image + ro->dst_start + ro->dst_offset;
  d_x_len = ro->dst_width;

  for(d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    src_2 = (const unsigned short *) src;
    dst_4 = (unsigned *) dst;
    for(s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len; ) {
      dst_4[d_x++] = bgr_2int(ro->dst_color_space, 5, 5, 5, src_2[s_x]);
      s_x += *(bre_x++);
    }
  }
}

/*
 * 15 bit true color --> 32 bit true color
 * Source format is BGR (see vesa.c:vbe_mode_info() )
 */
void gen_15to32_1(RemapObject *ro)
{
  int i, j;
  const unsigned char *src;
  unsigned char *dst;
  const unsigned short *src_2;
  unsigned *dst_4;

  src = ro->src_image + ro->src_start + ro->src_offset;
  dst = ro->dst_image + ro->dst_start + ro->dst_offset;

  for(i = ro->src_y0; i < ro->src_y1; i++) {
    src_2 = (const unsigned short *)src;
    dst_4 = (unsigned *) dst;

    for(j = 0; j < ro->dst_width; j++) {
      // get 5-bit color values
      // (green channel is cut between two byte values)
      //  [0] gggbbbbb
      //  [1] 0rrrrrgg
      *dst_4++ = bgr_2int(ro->dst_color_space, 5, 5, 5, *src_2++);
    }

    src += ro->src_scan_len;
    dst += ro->dst_scan_len;
  }
}

/*
 * 16 bit true color --> 16 bit true color
 * 15 bit true color --> 15 bit true color
 * Source format is BGR (see vesa.c:vbe_mode_info() )
 * *** ignores color space description ***
 */
void gen_16to16_1(RemapObject *ro)
{
  int i;
  const unsigned char *src;
  unsigned char *dst;

  src = ro->src_image + ro->src_start + ro->src_offset;
  dst = ro->dst_image + ro->dst_start + ro->dst_offset;

  for(i = ro->src_y0; i < ro->src_y1; i++) {
    memcpy(dst, src, ro->src_width << 1);
    src += ro->src_scan_len;
    dst += ro->dst_scan_len;
  }
}

/*
 * 16 bit true color --> 32 bit true color
 * supports arbitrary scaling
 */
void gen_16to32_all(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len;
  int *bre_x;
  int *bre_y = ro->bre_y;

  const unsigned char *src, *src0;
  unsigned char *dst;
  const unsigned short *src_2;
  unsigned *dst_4;

  src0 = ro->src_image + ro->src_start;
  dst = ro->dst_image + ro->dst_start + ro->dst_offset;
  d_x_len = ro->dst_width;

  for(d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    src_2 = (const unsigned short *) src;
    dst_4 = (unsigned *) dst;
    for(s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len; ) {
      dst_4[d_x++] = bgr_2int(ro->dst_color_space, 5, 6, 5, src_2[s_x]);
      s_x += *(bre_x++);
    }
  }
}

/*
 * 16 bit true color --> 32 bit true color
 * Source format is BGR (see vesa.c:vbe_mode_info() )
 */
void gen_16to32_1(RemapObject *ro)
{
  int i, j;
  const unsigned char *src;
  unsigned char *dst;
  const unsigned short *src_2;
  unsigned *dst_4;

  src = ro->src_image + ro->src_start + ro->src_offset;
  dst = ro->dst_image + ro->dst_start + ro->dst_offset;

  for(i = ro->src_y0; i < ro->src_y1; i++) {
    src_2 = (const unsigned short *)src;
    dst_4 = (unsigned *) dst;

    for(j = 0; j < ro->dst_width; j++) {
      // get 5-bit/6-bit color values
      // (green channel is cut between two byte values)
      //  [0] gggbbbbb
      //  [1] rrrrrggg
      *dst_4++ = bgr_2int(ro->dst_color_space, 5, 6, 5, *src_2++);
    }

    src += ro->src_scan_len;
    dst += ro->dst_scan_len;
  }
}

/*
 * 24 bit true color --> 24 bit true color
 * Source format is BGR (see vesa.c:vbe_mode_info() )
 * *** ignores color space description ***
 */
void gen_24to24_1(RemapObject *ro)
{
  int i;
  const unsigned char *src;
  unsigned char *dst;

  src = ro->src_image + ro->src_start + ro->src_offset;
  dst = ro->dst_image + ro->dst_start + ro->dst_offset;

  for(i = ro->src_y0; i < ro->src_y1; i++) {
    memcpy(dst, src, ro->src_width * 3);
    src += ro->src_scan_len;
    dst += ro->dst_scan_len;
  }
}

/*
 * 24 bit true color --> 32 bit true color
 * supports arbitrary scaling
 */
void gen_24to32_all(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len;
  int *bre_x;
  int *bre_y = ro->bre_y;

  const unsigned char *src, *src0;
  unsigned char *dst;
  unsigned *dst_4;

  src0 = ro->src_image + ro->src_start;
  dst = ro->dst_image + ro->dst_start + ro->dst_offset;
  d_x_len = ro->dst_width;

  for(d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    dst_4 = (unsigned *) dst;
    for(s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len; ) {
      RGBColor c = { src[s_x * 3 + 2], src[s_x * 3 + 1], src[s_x * 3] };
      dst_4[d_x++] = rgb_color_2int(ro->dst_color_space, 8, 8, 8, c);
      s_x += *(bre_x++);
    }
  }
}

/*
 * 24 bit true color --> 32 bit true color
 * Source format is BGR (see vesa.c:vbe_mode_info() )
 */
void gen_24to32_1(RemapObject *ro)
{
  int i, j;
  const unsigned char *src, *src_1;
  unsigned char *dst;
  unsigned *dst_4;

  src = ro->src_image + ro->src_start + ro->src_offset;
  dst = ro->dst_image + ro->dst_start + ro->dst_offset;

  for(i = ro->src_y0; i < ro->src_y1; i++) {
    src_1 = src;
    dst_4 = (unsigned *) dst;

    for(j = 0; j < ro->src_width; j++) {
      RGBColor c = { src_1[2], src_1[1], src_1[0] };

      *dst_4++ = rgb_color_2int(ro->dst_color_space, 8, 8, 8, c);
      src_1 += 3;
    }

    src += ro->src_scan_len;
    dst += ro->dst_scan_len;
  }
}

/*
 * 32 bit true color --> 32 bit true color
 * supports arbitrary scaling
 */
void gen_32to32_all(RemapObject *ro)
{
  int d_x_len;
  int s_x, d_x, d_y;
  int d_scan_len = ro->dst_scan_len;
  int *bre_x;
  int *bre_y = ro->bre_y;

  const unsigned char *src, *src0;
  unsigned char *dst;
  unsigned *dst_4;

  src0 = ro->src_image + ro->src_start;
  dst = ro->dst_image + ro->dst_start + ro->dst_offset;
  d_x_len = ro->dst_width;

  for(d_y = ro->dst_y0; d_y < ro->dst_y1; dst += d_scan_len) {
    src = src0 + bre_y[d_y++];
    dst_4 = (unsigned *) dst;
    for(s_x = d_x = 0, bre_x = ro->bre_x; d_x < d_x_len; ) {
      RGBColor c = { src[s_x * 4 + 2], src[s_x * 4 + 1], src[s_x * 4] };
      dst_4[d_x++] = rgb_color_2int(ro->dst_color_space, 8, 8, 8, c);
      s_x += *(bre_x++);
    }
  }
}

/*
 * 32 bit true color --> 32 bit true color
 * Source format is BGR (see vesa.c:vbe_mode_info() )
 * *** ignores color space description ***
 */
void gen_32to32_1(RemapObject *ro)
{
  int i;
  const unsigned char *src;
  unsigned char *dst;

  src = ro->src_image + ro->src_start + ro->src_offset;
  dst = ro->dst_image + ro->dst_start + ro->dst_offset;

  for(i = ro->src_y0; i < ro->src_y1; i++) {
    memcpy(dst, src, ro->src_width << 2);
    src += ro->src_scan_len;
    dst += ro->dst_scan_len;
  }
}

#define RO(p) (*(RemapObject **)p)

static RemapObject *re_create_obj(RemapObject *old, int new_mode)
{
  RemapObject *dst = _remap_init(new_mode, old->dst_mode,
    old->features, old->dst_color_space, old->gamma);
  if (old->color_lut_size && dst->color_lut_size == old->color_lut_size)
    memcpy(dst->true_color_lut, old->true_color_lut, dst->color_lut_size *
	sizeof(*dst->true_color_lut));
  else
    dirty_all_vga_colors();
  _remap_done(old);
  return dst;
}

static int _remap_palette_update(void *ros, unsigned i,
	unsigned bits, unsigned r, unsigned g, unsigned b)
{
  RemapObject *ro = RO(ros);
  return ro->palette_update(ro, i, bits, r, g, b);
}

static RectArea _remap_remap_rect(void *ros, const struct bitmap_desc src_img,
	int src_mode,
	int x0, int y0, int width, int height,
	struct bitmap_desc dst_img)
{
  RemapObject *ro = RO(ros);
  if (src_mode != ro->src_mode)
    RO(ros) = ro = re_create_obj(ro, src_mode);
  ro->src_image = src_img.img;
  ro->src_start = 0;
  ro->dst_image = dst_img.img;
  ro->src_resize(ro, src_img.width, src_img.height, src_img.scan_len);
  ro->dst_resize(ro, dst_img.width, dst_img.height, dst_img.scan_len);
  return ro->remap_rect(ro, x0, y0, width, height);
}

static RectArea _remap_remap_rect_dst(void *ros,
	const struct bitmap_desc src_img,
	int src_mode,
	int x0, int y0, int width, int height, struct bitmap_desc dst_img)
{
  RemapObject *ro = RO(ros);
  if (src_mode != ro->src_mode)
    RO(ros) = ro = re_create_obj(ro, src_mode);
  ro->src_image = src_img.img;
  ro->src_start = 0;
  ro->dst_image = dst_img.img;
  ro->src_resize(ro, src_img.width, src_img.height, src_img.scan_len);
  ro->dst_resize(ro, dst_img.width, dst_img.height, dst_img.scan_len);
  return ro->remap_rect_dst(ro, x0, y0, width, height);
}

static RectArea _remap_remap_mem(void *ros,
	const struct bitmap_desc src_img,
	int src_mode,
	int src_start, int offset, int len, struct bitmap_desc dst_img)
{
  RemapObject *ro = RO(ros);
  if (src_mode != ro->src_mode)
    RO(ros) = ro = re_create_obj(ro, src_mode);
  ro->src_image = src_img.img;
  ro->src_start = src_start;
  ro->dst_image = dst_img.img;
  ro->src_resize(ro, src_img.width, src_img.height, src_img.scan_len);
  ro->dst_resize(ro, dst_img.width, dst_img.height, dst_img.scan_len);
  return ro->remap_mem(ro, offset, len);
}

static int _remap_get_cap(void *ros)
{
  RemapObject *ro = RO(ros);
  return ro->state;
}

static void *_remap_remap_init(int dst_mode, int features,
        const ColorSpaceDesc *color_space, int gamma)
{
  RemapObject **p, *o;
  p = malloc(sizeof(*p));
  o = malloc(sizeof(*o));
  /* create dummy remap. init properly later, when src mode is known */
  memset(o, 0, sizeof(*o));
  o->dst_mode = dst_mode;
  o->features = features;
  o->dst_color_space = color_space;
  o->palette_update = do_nearly_nothing;
  o->gamma = gamma;
  *p = o;
  return p;
}

static void _remap_remap_done(void *ros)
{
  RemapObject *ro = RO(ros);
  _remap_done(ro);
  free(ros);
}

static struct remap_calls rmcalls = {
  _remap_remap_init,
  _remap_remap_done,
  _remap_palette_update,
  _remap_remap_rect,
  _remap_remap_rect_dst,
  _remap_remap_mem,
  _remap_get_cap,
  "dosemu gfx remapper"
};

void remapper_register(void)
{
  register_remapper(&rmcalls, REMAP_DOSEMU);
}

/*
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 *
 *                     some test remap functions
 *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 */

#ifdef REMAP_TEST

void test_8to32_(RemapObject *);

static RemapFuncDesc remap_test_list[] = {

};

/*
 * returns chained list of modes
 */
RemapFuncDesc *remap_test(void)
{
  int i;

  for(i = 0; i < sizeof(remap_test_list) / sizeof(*remap_test_list) - 1; i++) {
    remap_test_list[i].next = remap_test_list + i + 1;
  }

  return remap_test_list;
}


#endif
