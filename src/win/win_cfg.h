#ifndef WIN_CFG_H
#define WIN_CFG_H
#define GP3RT_VERSION "1.0.1"
typedef struct {
    int windowed, window_scale, software, internal_scale, filtering, anisotropy, vsync, stretch;
    int virtual_cd, multiplayer, lazy_readback;
    int emulate_ctx;   /* [debug] thread_bound */
} win_cfg_t;
extern win_cfg_t g_wcfg;
extern char g_game_dir[1024];
void ExitProcess_(unsigned code);
#endif
