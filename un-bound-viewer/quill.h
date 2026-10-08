#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define QUILL_CONTENT_MONO 0
#define QUILL_CONTENT_COLOR 1

#define QUILL_MODE_FASTEST 0
#define QUILL_MODE_FAST 1
#define QUILL_MODE_COLOR3 3
#define QUILL_MODE_COLOR4 4
#define QUILL_MODE_COLOR5 5

int quill_init(void);
int quill_width(void);
int quill_height(void);
int quill_stride(void);
int quill_format(void);
unsigned char *quill_buffer(void);
unsigned long quill_swap_ex(int x, int y, int w, int h,
                            int mode, int full_refresh, int content_type);
unsigned long quill_swap(int x, int y, int w, int h, int mode, int full_refresh);
unsigned long quill_swap_mono_fast(int x, int y, int w, int h);
unsigned long quill_swap_mono_quality(int x, int y, int w, int h);
unsigned long quill_swap_color(int x, int y, int w, int h);
unsigned long quill_swap_color_full(int x, int y, int w, int h);
void quill_process_events(void);

#ifdef __cplusplus
}
#endif
