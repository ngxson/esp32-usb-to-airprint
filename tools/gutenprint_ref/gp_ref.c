/* Reference: render a synthetic RGB image through Gutenprint's MG2500 driver */
#include <gutenprint/gutenprint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int W, H;
static FILE *out;

static void outfn(void *data, const char *buf, size_t n) { fwrite(buf, 1, n, (FILE *)data); }
static void errfn(void *data, const char *buf, size_t n) { fwrite(buf, 1, n, stderr); }

static void im_init(stp_image_t *i) {}
static void im_reset(stp_image_t *i) {}
static int im_w(stp_image_t *i) { return W; }
static int im_h(stp_image_t *i) { return H; }
static const char *im_app(stp_image_t *i) { return "ref"; }
static void im_conc(stp_image_t *i) {}
static stp_image_status_t im_row(stp_image_t *i, unsigned char *d, size_t lim, int row)
{
  /* 6 bands: white, cyan, magenta, yellow, black, white ; left half solid, right half ramp */
  int band = row * 6 / H;
  static const unsigned char col[6][3] = {{255,255,255},{0,255,255},{255,0,255},{255,255,0},{0,0,0},{255,255,255}};
  for (int x = 0; x < W; x++) {
    for (int c = 0; c < 3; c++) {
      int v = col[band][c];
      if (x >= W / 2) v = 255 - ((255 - v) * (W - 1 - x) / (W / 2));
      d[x * 3 + c] = v;
    }
  }
  return STP_IMAGE_STATUS_OK;
}

int main(int argc, char **argv)
{
  /* usage: gp_ref <resolution> <out.prn> [PageSize] [pages] */
  const char *res = argc > 1 ? argv[1] : "300x300dpi";
  const char *pagesize = argc > 3 ? argv[3] : "A4";
  int pages = argc > 4 ? atoi(argv[4]) : 1;
  out = fopen(argc > 2 ? argv[2] : "ref.prn", "wb");
  stp_init();
  const stp_printer_t *p = stp_get_printer_by_driver("bjc-MG2500-series");
  if (!p) { fprintf(stderr, "no printer\n"); return 1; }
  stp_vars_t *v = stp_vars_create_copy(stp_printer_get_defaults(p));
  stp_set_driver(v, "bjc-MG2500-series"); stp_set_printer_defaults(v, p);
  stp_set_outfunc(v, outfn); stp_set_outdata(v, out);
  stp_set_errfunc(v, errfn); stp_set_errdata(v, stderr);
  stp_set_string_parameter(v, "PageSize", pagesize); stp_set_string_parameter(v, "JobMode", "Job");
  stp_set_string_parameter(v, "MediaType", "Plain");
  stp_set_string_parameter(v, "Resolution", res);
  stp_set_string_parameter(v, "PrintingMode", "Color");
  stp_set_string_parameter(v, "InputImageType", "RGB");
  stp_dimension_t pw, ph, l, r, b, t;
  stp_get_media_size(v, &pw, &ph);
  stp_set_page_width(v, pw); stp_set_page_height(v, ph);
  stp_get_imageable_area(v, &l, &r, &b, &t);
  fprintf(stderr, "page %gx%g pt, imageable l=%g r=%g t=%g b=%g\n", pw, ph, l, r, t, b);
  stp_set_left(v, l); stp_set_top(v, t);
  stp_set_width(v, r - l); stp_set_height(v, b - t);
  W = (int)((r - l) * 300 / 72); H = (int)((b - t) * 300 / 72);
  stp_image_t im = { im_init, im_reset, im_w, im_h, im_row, im_app, im_conc, NULL };
  if (!stp_verify(v)) { fprintf(stderr, "verify failed\n"); return 1; }
  stp_start_job(v, &im);
  int ok = 1;
  for (int pg = 0; pg < pages && ok; pg++) {
    stp_set_int_parameter(v, "PageNumber", pg);
    ok = stp_print(v, &im);
  }
  stp_end_job(v, &im);
  fclose(out);
  fprintf(stderr, "print ok=%d, image %dx%d\n", ok, W, H);
  return !ok;
}
