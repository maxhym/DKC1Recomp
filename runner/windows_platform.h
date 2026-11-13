#ifndef DKC1_WINDOWS_PLATFORM_H
#define DKC1_WINDOWS_PLATFORM_H
#include <SDL.h>
#include "desktop_graphics.h"
void Dkc1WindowsAttach(SDL_Window *window);
void Dkc1WindowsEvent(const SDL_Event *event);
void Dkc1WindowsDetach(void);
void Dkc1WindowsShowMenuBar(int visible);
int Dkc1WindowsMenuBarVisible(void);
int Dkc1WindowsSavedHaptics(void);
void Dkc1WindowsSetHaptics(int enabled);
void Dkc1WindowsUpdateHapticsMenu(int enabled);
bool Dkc1WindowsGraphicsInit(SDL_Window *window);
/* True when the presenter can draw the HD character sprite. */
bool Dkc1WindowsGraphicsSpritesReady(void);
void Dkc1WindowsGraphicsDraw(const uint32_t *pixels,int w,int h,int display_width,
                             const Dkc1GraphicsSettings *settings);
void Dkc1WindowsGraphicsSwap(void);
void Dkc1WindowsGraphicsClose(void);
int Dkc1WindowsGraphicsTest(void);
int Dkc1WindowsPlatformTest(const char *directory);
#endif
