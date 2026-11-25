/* SDL scancode -> Windows virtual key + PC set-1 scancode (0x100 = extended key) */
#ifndef KEYS_H
#define KEYS_H
#include <SDL.h>
static int sdl_to_vk(SDL_Scancode s, int *scan) {
    int vk = 0, sc = 0;
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) {
        static const unsigned char sca[26] = { 0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c };
        vk = 'A' + (s - SDL_SCANCODE_A); sc = sca[s - SDL_SCANCODE_A];
    } else if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) { vk = '1' + (s - SDL_SCANCODE_1); sc = 0x02 + (s - SDL_SCANCODE_1); }
    else if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F10) { vk = 0x70 + (s - SDL_SCANCODE_F1); sc = 0x3b + (s - SDL_SCANCODE_F1); }
    else if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9) {
        static const unsigned char kps[9] = { 0x4f,0x50,0x51,0x4b,0x4c,0x4d,0x47,0x48,0x49 };
        vk = 0x61 + (s - SDL_SCANCODE_KP_1); sc = kps[s - SDL_SCANCODE_KP_1];
    } else switch (s) {
        case SDL_SCANCODE_0: vk = '0'; sc = 0x0b; break;
        case SDL_SCANCODE_RETURN: vk = 0x0d; sc = 0x1c; break;
        case SDL_SCANCODE_ESCAPE: vk = 0x1b; sc = 0x01; break;
        case SDL_SCANCODE_BACKSPACE: vk = 0x08; sc = 0x0e; break;
        case SDL_SCANCODE_TAB: vk = 0x09; sc = 0x0f; break;
        case SDL_SCANCODE_SPACE: vk = 0x20; sc = 0x39; break;
        case SDL_SCANCODE_MINUS: vk = 0xbd; sc = 0x0c; break;
        case SDL_SCANCODE_EQUALS: vk = 0xbb; sc = 0x0d; break;
        case SDL_SCANCODE_LEFTBRACKET: vk = 0xdb; sc = 0x1a; break;
        case SDL_SCANCODE_RIGHTBRACKET: vk = 0xdd; sc = 0x1b; break;
        case SDL_SCANCODE_BACKSLASH: vk = 0xdc; sc = 0x2b; break;
        case SDL_SCANCODE_NONUSBACKSLASH: vk = 0xe2; sc = 0x56; break;
        case SDL_SCANCODE_SEMICOLON: vk = 0xba; sc = 0x27; break;
        case SDL_SCANCODE_APOSTROPHE: vk = 0xde; sc = 0x28; break;
        case SDL_SCANCODE_GRAVE: vk = 0xc0; sc = 0x29; break;
        case SDL_SCANCODE_COMMA: vk = 0xbc; sc = 0x33; break;
        case SDL_SCANCODE_PERIOD: vk = 0xbe; sc = 0x34; break;
        case SDL_SCANCODE_SLASH: vk = 0xbf; sc = 0x35; break;
        case SDL_SCANCODE_INTERNATIONAL1: vk = 0xc1; sc = 0x73; break;   /* ABNT2 /? key */
        case SDL_SCANCODE_KP_COMMA: vk = 0xc2; sc = 0x7e; break;        /* ABNT2 keypad . */
        case SDL_SCANCODE_CAPSLOCK: vk = 0x14; sc = 0x3a; break;
        case SDL_SCANCODE_F11: vk = 0x7a; sc = 0x57; break;
        case SDL_SCANCODE_F12: vk = 0x7b; sc = 0x58; break;
        case SDL_SCANCODE_PRINTSCREEN: vk = 0x2c; sc = 0x137; break;
        case SDL_SCANCODE_SCROLLLOCK: vk = 0x91; sc = 0x46; break;
        case SDL_SCANCODE_PAUSE: vk = 0x13; sc = 0x45; break;
        case SDL_SCANCODE_INSERT: vk = 0x2d; sc = 0x152; break;
        case SDL_SCANCODE_HOME: vk = 0x24; sc = 0x147; break;
        case SDL_SCANCODE_PAGEUP: vk = 0x21; sc = 0x149; break;
        case SDL_SCANCODE_DELETE: vk = 0x2e; sc = 0x153; break;
        case SDL_SCANCODE_END: vk = 0x23; sc = 0x14f; break;
        case SDL_SCANCODE_PAGEDOWN: vk = 0x22; sc = 0x151; break;
        case SDL_SCANCODE_RIGHT: vk = 0x27; sc = 0x14d; break;
        case SDL_SCANCODE_LEFT: vk = 0x25; sc = 0x14b; break;
        case SDL_SCANCODE_DOWN: vk = 0x28; sc = 0x150; break;
        case SDL_SCANCODE_UP: vk = 0x26; sc = 0x148; break;
        case SDL_SCANCODE_NUMLOCKCLEAR: vk = 0x90; sc = 0x145; break;
        case SDL_SCANCODE_KP_DIVIDE: vk = 0x6f; sc = 0x135; break;
        case SDL_SCANCODE_KP_MULTIPLY: vk = 0x6a; sc = 0x37; break;
        case SDL_SCANCODE_KP_MINUS: vk = 0x6d; sc = 0x4a; break;
        case SDL_SCANCODE_KP_PLUS: vk = 0x6b; sc = 0x4e; break;
        case SDL_SCANCODE_KP_ENTER: vk = 0x0d; sc = 0x11c; break;
        case SDL_SCANCODE_KP_0: vk = 0x60; sc = 0x52; break;
        case SDL_SCANCODE_KP_PERIOD: vk = 0x6e; sc = 0x53; break;
        case SDL_SCANCODE_LCTRL: vk = 0x11; sc = 0x1d; break;
        case SDL_SCANCODE_RCTRL: vk = 0x11; sc = 0x11d; break;
        case SDL_SCANCODE_LSHIFT: vk = 0x10; sc = 0x2a; break;
        case SDL_SCANCODE_RSHIFT: vk = 0x10; sc = 0x36; break;
        case SDL_SCANCODE_LALT: vk = 0x12; sc = 0x38; break;
        case SDL_SCANCODE_RALT: vk = 0x12; sc = 0x138; break;
        case SDL_SCANCODE_LGUI: vk = 0x5b; sc = 0x15b; break;
        case SDL_SCANCODE_RGUI: vk = 0x5c; sc = 0x15c; break;
        case SDL_SCANCODE_APPLICATION: vk = 0x5d; sc = 0x15d; break;
        default: break;
    }
    *scan = sc;
    return vk;
}
#endif
