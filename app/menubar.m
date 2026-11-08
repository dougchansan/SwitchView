// Native macOS menu bar (SwitchView / Edit / View). The items synthesize the
// keys the rest of the app already understands, so there is one source of truth:
// Copy/Select All drive the log-box selection in panel.c, Paste feeds the text
// box, and Full Screen reuses main.c's F11 toggle. SDL links Cocoa already.
#import <Cocoa/Cocoa.h>
#include <SDL.h>

static void push_key(SDL_Keycode sym, Uint16 mod)
{
  SDL_Event e;
  SDL_zero(e);
  e.type = SDL_KEYDOWN;
  e.key.state = SDL_PRESSED;
  e.key.keysym.sym = sym;
  e.key.keysym.scancode = SDL_GetScancodeFromKey(sym);
  e.key.keysym.mod = mod;
  e.key.timestamp = SDL_GetTicks();
  SDL_PushEvent(&e);
}

@interface SVMenuTarget : NSObject
@end

@implementation SVMenuTarget
- (void)svCopy:(id)sender { (void)sender; push_key(SDLK_c, KMOD_GUI); }
- (void)svPaste:(id)sender { (void)sender; push_key(SDLK_v, KMOD_GUI); }
- (void)svSelectAll:(id)sender { (void)sender; push_key(SDLK_a, KMOD_GUI); }
- (void)svFullScreen:(id)sender { (void)sender; push_key(SDLK_F11, KMOD_NONE); }
@end

static SVMenuTarget *g_target;  // kept alive for the app's lifetime

void macos_menubar_install(void)
{
  if (!NSApp) return;
  g_target = [[SVMenuTarget alloc] init];

  NSMenu *bar = [[NSMenu alloc] init];
  [NSApp setMainMenu:bar];

  // Application menu (shown under the app's name, in bold).
  NSMenuItem *appItem = [[NSMenuItem alloc] init];
  [bar addItem:appItem];
  NSMenu *appMenu = [[NSMenu alloc] init];
  [appItem setSubmenu:appMenu];
  [appMenu addItemWithTitle:@"About SwitchView"
                     action:@selector(orderFrontStandardAboutPanel:)
              keyEquivalent:@""];
  [appMenu addItem:[NSMenuItem separatorItem]];
  [appMenu addItemWithTitle:@"Hide SwitchView" action:@selector(hide:) keyEquivalent:@"h"];
  NSMenuItem *others = [appMenu addItemWithTitle:@"Hide Others"
                                          action:@selector(hideOtherApplications:)
                                   keyEquivalent:@"h"];
  [others setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagOption];
  [appMenu addItem:[NSMenuItem separatorItem]];
  [appMenu addItemWithTitle:@"Quit SwitchView" action:@selector(terminate:) keyEquivalent:@"q"];

  // Edit menu.
  NSMenuItem *editItem = [[NSMenuItem alloc] init];
  [bar addItem:editItem];
  NSMenu *editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
  [editItem setSubmenu:editMenu];
  NSMenuItem *mi;
  mi = [editMenu addItemWithTitle:@"Copy" action:@selector(svCopy:) keyEquivalent:@"c"];
  [mi setTarget:g_target];
  mi = [editMenu addItemWithTitle:@"Paste" action:@selector(svPaste:) keyEquivalent:@"v"];
  [mi setTarget:g_target];
  [editMenu addItem:[NSMenuItem separatorItem]];
  mi = [editMenu addItemWithTitle:@"Select All" action:@selector(svSelectAll:) keyEquivalent:@"a"];
  [mi setTarget:g_target];

  // View menu.
  NSMenuItem *viewItem = [[NSMenuItem alloc] init];
  [bar addItem:viewItem];
  NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
  [viewItem setSubmenu:viewMenu];
  mi = [viewMenu addItemWithTitle:@"Toggle Full Screen" action:@selector(svFullScreen:) keyEquivalent:@"f"];
  [mi setKeyEquivalentModifierMask:NSEventModifierFlagControl | NSEventModifierFlagCommand];
  [mi setTarget:g_target];
}
