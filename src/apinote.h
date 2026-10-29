/* Census of DirectDraw/Direct3D calls by calling module (diagnostics, log_level >= 2):
 * the first call of each method from each module is logged, e.g. to see what an
 * add-on such as GPxPatch does with the device. */
#ifndef APINOTE_H
#define APINOTE_H
void api_note(const char *name, void *caller);
extern int g_log_level;
#define API_NOTE(n) do { if (g_log_level >= 2) api_note(n, __builtin_return_address(0)); } while (0)
#endif
