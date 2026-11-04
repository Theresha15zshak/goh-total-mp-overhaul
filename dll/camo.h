#pragma once
#ifdef GOH_CAMO
#ifdef __cplusplus
struct ID3D11Device;
extern "C" {
#endif
void camo_init(const char *dir);
int camo_open(void *path, unsigned mode, void *(*prev)(void *, unsigned), void **out);
void camo_install(unsigned char *exe, unsigned char *(*alloc)(size_t), void (*patch)(void *, const void *, size_t));
void camo_page(void *page);
void camo_tick(void);
int camo_shown(void);
#ifdef __cplusplus
void camo_draw(ID3D11Device *device, void *window);
}
#endif
#else
#define camo_init(dir) ((void)0)
#define camo_open(path, mode, prev, out) 0
#define camo_install(exe, alloc, patch) ((void)0)
#define camo_page(page) ((void)0)
#define camo_tick() ((void)0)
#define camo_shown() 0
#define camo_draw(device, window) ((void)0)
#endif
