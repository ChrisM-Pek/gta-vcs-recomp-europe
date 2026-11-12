// The platform layer on macOS (docs/LAUNCHER.md "macOS"): Cocoa, one NSWindow whose content view is backed by a
// CAMetalLayer (render/metal). Objective-C++ with ARC.
//
// The window keeps macOS' own frame: the title bar is transparent and the content runs under it (full-size content
// view), so the UI draws its own title bar as on Windows and Linux while the traffic lights, resizing, snapping and
// full screen stay native. A press on the caption the UI reports (setChrome) drags the window; a double click there
// does what System Settings says (zoom or minimise).
//
// Keys are translated to Windows virtual-key values; Command counts as Ctrl for the UI's shortcuts (Cmd+L, Cmd+C...).
// Waiting is NSApp's event queue (wake() posts an application-defined event); while macOS runs a loop of its own - a
// live resize, an open / save panel - a timer in those run loop modes keeps frames coming (setModalFrame).
#include "platform.h"

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h> // kVK_* key codes
#import <QuartzCore/CAMetalLayer.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <ctime>

#if !__has_feature(objc_arc)
#error "platform_macos.mm is built with ARC (-fobjc-arc)"
#endif

namespace platform
{
    Input input;
    Window window;

    namespace
    {
        Chrome g_chrome;
        void (*g_modalFrame)() = nullptr;
        bool g_inModalFrame = false;
        Cursor g_cursor = Cursor::Arrow;
        double g_lastClickTime = -1;
        float g_lastClickX = 0, g_lastClickY = 0;
        NSWindow *g_window = nil;
        NSView *g_view = nil;
        CAMetalLayer *g_layer = nil;
        id g_delegate = nil;

        struct PixelPoint
        {
            float x, y;
        };

        void runModalFrame()
        {
            if (!g_modalFrame || g_inModalFrame)
                return;
            g_inModalFrame = true;
            g_modalFrame();
            g_inModalFrame = false;
        }

        int virtualKey(unsigned short code)
        {
            switch (code)
            {
            case kVK_Delete: return kKeyBackspace;
            case kVK_ForwardDelete: return 0x2E; // VK_DELETE
            case kVK_Tab: return kKeyTab;
            case kVK_Return:
            case kVK_ANSI_KeypadEnter: return kKeyEnter;
            case kVK_Shift:
            case kVK_RightShift: return kKeyShift;
            case kVK_Control:
            case kVK_RightControl:
            case kVK_Command:
            case kVK_RightCommand: return kKeyControl;
            case kVK_Option:
            case kVK_RightOption: return kKeyAlt;
            case kVK_Escape: return kKeyEscape;
            case kVK_Space: return kKeySpace;
            case kVK_PageUp: return kKeyPageUp;
            case kVK_PageDown: return kKeyPageDown;
            case kVK_End: return kKeyEnd;
            case kVK_Home: return kKeyHome;
            case kVK_LeftArrow: return kKeyLeft;
            case kVK_UpArrow: return kKeyUp;
            case kVK_RightArrow: return kKeyRight;
            case kVK_DownArrow: return kKeyDown;
            case kVK_F1: return 0x70;
            case kVK_F2: return 0x71;
            case kVK_F3: return 0x72;
            case kVK_F4: return 0x73;
            case kVK_F5: return 0x74;
            case kVK_F6: return 0x75;
            case kVK_F7: return 0x76;
            case kVK_F8: return 0x77;
            case kVK_F9: return 0x78;
            case kVK_F10: return 0x79;
            case kVK_F11: return 0x7A;
            case kVK_F12: return 0x7B;
            case kVK_ANSI_A: return 'A';
            case kVK_ANSI_B: return 'B';
            case kVK_ANSI_C: return 'C';
            case kVK_ANSI_D: return 'D';
            case kVK_ANSI_E: return 'E';
            case kVK_ANSI_F: return 'F';
            case kVK_ANSI_G: return 'G';
            case kVK_ANSI_H: return 'H';
            case kVK_ANSI_I: return 'I';
            case kVK_ANSI_J: return 'J';
            case kVK_ANSI_K: return 'K';
            case kVK_ANSI_L: return 'L';
            case kVK_ANSI_M: return 'M';
            case kVK_ANSI_N: return 'N';
            case kVK_ANSI_O: return 'O';
            case kVK_ANSI_P: return 'P';
            case kVK_ANSI_Q: return 'Q';
            case kVK_ANSI_R: return 'R';
            case kVK_ANSI_S: return 'S';
            case kVK_ANSI_T: return 'T';
            case kVK_ANSI_U: return 'U';
            case kVK_ANSI_V: return 'V';
            case kVK_ANSI_W: return 'W';
            case kVK_ANSI_X: return 'X';
            case kVK_ANSI_Y: return 'Y';
            case kVK_ANSI_Z: return 'Z';
            case kVK_ANSI_0: return '0';
            case kVK_ANSI_1: return '1';
            case kVK_ANSI_2: return '2';
            case kVK_ANSI_3: return '3';
            case kVK_ANSI_4: return '4';
            case kVK_ANSI_5: return '5';
            case kVK_ANSI_6: return '6';
            case kVK_ANSI_7: return '7';
            case kVK_ANSI_8: return '8';
            case kVK_ANSI_9: return '9';
            default: return 0;
            }
        }

        void readSettings()
        {
            NSAppearanceName best = [[NSApp effectiveAppearance]
                bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]];
            window.darkMode = [best isEqualToString:NSAppearanceNameDarkAqua];
            window.reducedMotion = [[NSWorkspace sharedWorkspace] accessibilityDisplayShouldReduceMotion];
            window.doubleClickSeconds = float([NSEvent doubleClickInterval]);
            if (NSScreen *screen = g_window ? [g_window screen] : [NSScreen mainScreen])
            {
                if (@available(macOS 12.0, *))
                    window.refreshHz = float(std::max<NSInteger>([screen maximumFramesPerSecond], 30));
            }
            window.needsRedraw = true;
        }

        void updateSize()
        {
            if (!g_view)
                return;
            const CGFloat scale = [g_window backingScaleFactor];
            const NSSize size = [g_view bounds].size;
            window.dpiScale = float(scale);
            window.width = int(std::lround(size.width * scale));
            window.height = int(std::lround(size.height * scale));
            window.maximized = [g_window isZoomed];
            window.minimized = [g_window isMiniaturized];
            g_layer.contentsScale = scale;
            window.needsRedraw = true;
        }

        // a view point (bottom-left origin, points) as the UI's client pixels (top-left origin)
        PixelPoint toPixels(NSPoint p)
        {
            const CGFloat scale = [g_window backingScaleFactor];
            const NSSize size = [g_view bounds].size;
            return {float(p.x * scale), float((size.height - p.y) * scale)};
        }

        bool inChrome(float x, float y)
        {
            if (g_chrome.maximizeButton.contains(x, y))
                return false;
            for (const core::Rect &r : g_chrome.buttons)
                if (r.contains(x, y))
                    return false;
            return g_chrome.caption.contains(x, y);
        }

        NSCursor *cursorFor(Cursor c)
        {
            switch (c)
            {
            case Cursor::Hand: return [NSCursor pointingHandCursor];
            case Cursor::Text: return [NSCursor IBeamCursor];
            default: return [NSCursor arrowCursor];
            }
        }

        void appendText(NSString *s)
        {
            const NSUInteger n = [s length];
            for (NSUInteger i = 0; i < n; ++i)
            {
                char32_t cp = [s characterAtIndex:i];
                if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n)
                {
                    const unichar low = [s characterAtIndex:i + 1];
                    if (low >= 0xDC00 && low < 0xE000)
                    {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        ++i;
                    }
                }
                // printable only (the function keys arrive as private-use characters)
                if (cp >= 32 && cp != 127 && !(cp >= 0xF700 && cp <= 0xF8FF))
                    input.text += cp;
            }
            window.needsRedraw = true;
        }

        void modifiers(NSEventModifierFlags flags)
        {
            input.ctrl = (flags & (NSEventModifierFlagControl | NSEventModifierFlagCommand)) != 0;
            input.shift = (flags & NSEventModifierFlagShift) != 0;
            input.alt = (flags & NSEventModifierFlagOption) != 0;
            input.keyDown[kKeyControl] = input.ctrl;
            input.keyDown[kKeyShift] = input.shift;
            input.keyDown[kKeyAlt] = input.alt;
        }

        void button(int b, bool down, NSEvent *event)
        {
            const PixelPoint p = toPixels([g_view convertPoint:[event locationInWindow] fromView:nil]);
            input.mouseMoved = input.mouseMoved || p.x != input.mouseX || p.y != input.mouseY;
            input.mouseX = p.x;
            input.mouseY = p.y;
            if (down && !input.down[b])
            {
                input.pressed[b] = true;
                if (b == kMouseLeft)
                {
                    const double t = now();
                    const float slop = 4.0f * window.dpiScale;
                    const bool second = t - g_lastClickTime <= window.doubleClickSeconds && std::fabs(p.x - g_lastClickX) <= slop &&
                                        std::fabs(p.y - g_lastClickY) <= slop;
                    input.clicks = second ? 2 : 1;
                    g_lastClickTime = second ? -1 : t;
                    g_lastClickX = p.x;
                    g_lastClickY = p.y;
                }
            }
            if (!down && input.down[b])
                input.released[b] = true;
            input.down[b] = down;
            window.needsRedraw = true;
        }

        std::vector<std::filesystem::path> fileUrls(id<NSDraggingInfo> info)
        {
            std::vector<std::filesystem::path> out;
            NSArray<NSURL *> *urls = [[info draggingPasteboard] readObjectsForClasses:@[ [NSURL class] ]
                                                                              options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
            for (NSURL *u in urls)
                if (const char *p = [u fileSystemRepresentation])
                    out.emplace_back(p);
            return out;
        }
    }
}

using namespace platform;

@interface VCSView : NSView <NSDraggingDestination>
@end

@implementation VCSView
- (BOOL)wantsUpdateLayer
{
    return YES;
}
- (CALayer *)makeBackingLayer
{
    CAMetalLayer *layer = [CAMetalLayer layer];
    g_layer = layer;
    return layer;
}
- (BOOL)acceptsFirstResponder
{
    return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent *)event
{
    return YES;
}
- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    for (NSTrackingArea *a in [self trackingAreas])
        [self removeTrackingArea:a];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:[self bounds]
                                                       options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
                                                               NSTrackingActiveAlways | NSTrackingInVisibleRect | NSTrackingCursorUpdate
                                                         owner:self
                                                      userInfo:nil]];
}
- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    updateSize();
}
- (void)viewDidChangeEffectiveAppearance
{
    readSettings();
}
- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    updateSize();
    if ([self inLiveResize])
        runModalFrame(); // macOS' resize loop starves the main loop: draw from here
}
- (void)moved:(NSEvent *)event
{
    const PixelPoint p = toPixels([self convertPoint:[event locationInWindow] fromView:nil]);
    const bool held = input.down[0] || input.down[1] || input.down[2];
    if (!held && inChrome(p.x, p.y))
    {
        if (input.mouseInside)
        {
            input.mouseInside = false;
            input.mouseX = input.mouseY = -1e6f;
            input.mouseMoved = true;
            window.needsRedraw = true;
        }
        [[NSCursor arrowCursor] set];
        return;
    }
    input.mouseMoved = input.mouseMoved || p.x != input.mouseX || p.y != input.mouseY;
    input.mouseX = p.x;
    input.mouseY = p.y;
    input.mouseInside = true;
    window.needsRedraw = true;
    [cursorFor(g_cursor) set];
}
- (void)mouseMoved:(NSEvent *)event
{
    [self moved:event];
}
- (void)mouseDragged:(NSEvent *)event
{
    [self moved:event];
}
- (void)rightMouseDragged:(NSEvent *)event
{
    [self moved:event];
}
- (void)otherMouseDragged:(NSEvent *)event
{
    [self moved:event];
}
- (void)cursorUpdate:(NSEvent *)event
{
    [cursorFor(g_cursor) set];
}
- (void)mouseEntered:(NSEvent *)event
{
    [self moved:event];
}
- (void)mouseExited:(NSEvent *)event
{
    input.mouseInside = false;
    input.mouseX = input.mouseY = -1e6f;
    input.mouseMoved = true;
    window.needsRedraw = true;
}
- (void)mouseDown:(NSEvent *)event
{
    const PixelPoint p = toPixels([self convertPoint:[event locationInWindow] fromView:nil]);
    if (inChrome(p.x, p.y))
    {
        if ([event clickCount] == 2)
        {
            // what System Settings > Desktop & Dock says a double-clicked title bar does
            NSString *action = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleActionOnDoubleClick"];
            if ([action isEqualToString:@"Minimize"])
                [g_window miniaturize:nil];
            else if (![action isEqualToString:@"None"])
                [g_window zoom:nil];
        }
        else
            [g_window performWindowDragWithEvent:event];
        return;
    }
    button(kMouseLeft, true, event);
}
- (void)mouseUp:(NSEvent *)event
{
    button(kMouseLeft, false, event);
}
- (void)rightMouseDown:(NSEvent *)event
{
    button(kMouseRight, true, event);
}
- (void)rightMouseUp:(NSEvent *)event
{
    button(kMouseRight, false, event);
}
- (void)otherMouseDown:(NSEvent *)event
{
    if ([event buttonNumber] == 2)
        button(kMouseMiddle, true, event);
}
- (void)otherMouseUp:(NSEvent *)event
{
    if ([event buttonNumber] == 2)
        button(kMouseMiddle, false, event);
}
- (void)scrollWheel:(NSEvent *)event
{
    // notches, positive away from the user; a trackpad's precise deltas are points (about 10 a notch)
    const CGFloat dy = [event scrollingDeltaY];
    input.wheel += float([event hasPreciseScrollingDeltas] ? dy / 10.0 : dy);
    window.needsRedraw = true;
}
- (void)keyDown:(NSEvent *)event
{
    modifiers([event modifierFlags]);
    const int vk = virtualKey([event keyCode]);
    if (vk > 0 && vk < 256)
    {
        input.keyDown[vk] = true;
        input.keyPressed[vk] = true; // a repeat is a press again, as Windows reports it
    }
    window.needsRedraw = true;
    if (!([event modifierFlags] & (NSEventModifierFlagCommand | NSEventModifierFlagControl)))
        [self interpretKeyEvents:@[ event ]]; // typed text (dead keys composed)
}
- (void)keyUp:(NSEvent *)event
{
    const int vk = virtualKey([event keyCode]);
    if (vk > 0 && vk < 256)
        input.keyDown[vk] = false;
    window.needsRedraw = true;
}
- (void)flagsChanged:(NSEvent *)event
{
    const bool ctrlBefore = input.ctrl, shiftBefore = input.shift, altBefore = input.alt;
    modifiers([event modifierFlags]);
    if (input.ctrl && !ctrlBefore)
        input.keyPressed[kKeyControl] = true;
    if (input.shift && !shiftBefore)
        input.keyPressed[kKeyShift] = true;
    if (input.alt && !altBefore)
        input.keyPressed[kKeyAlt] = true;
    window.needsRedraw = true;
}
- (void)insertText:(id)string
{
    appendText([string isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString *)string string] : (NSString *)string);
}
- (void)doCommandBySelector:(SEL)selector
{
    // Enter, Tab, arrows...: keyDown reported them already; no beep
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    input.dragOver = true;
    return [self draggingUpdated:sender];
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender
{
    const PixelPoint p = toPixels([self convertPoint:[sender draggingLocation] fromView:nil]);
    input.dragX = p.x;
    input.dragY = p.y;
    window.needsRedraw = true;
    return NSDragOperationCopy;
}
- (void)draggingExited:(id<NSDraggingInfo>)sender
{
    input.dragOver = false;
    window.needsRedraw = true;
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    input.dragOver = false;
    for (auto &p : fileUrls(sender))
        input.dropped.push_back(p);
    window.needsRedraw = true;
    return YES;
}
@end

@interface VCSWindowDelegate : NSObject <NSWindowDelegate>
@end

@implementation VCSWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender
{
    window.quitRequested = true; // the UI decides
    window.needsRedraw = true;
    return NO;
}
- (void)windowDidResize:(NSNotification *)n
{
    updateSize();
}
- (void)windowDidChangeBackingProperties:(NSNotification *)n
{
    updateSize();
}
- (void)windowDidChangeScreen:(NSNotification *)n
{
    readSettings();
}
- (void)windowDidMiniaturize:(NSNotification *)n
{
    window.minimized = true;
    window.needsRedraw = true;
}
- (void)windowDidDeminiaturize:(NSNotification *)n
{
    window.minimized = false;
    window.needsRedraw = true;
}
- (void)windowDidBecomeKey:(NSNotification *)n
{
    window.focused = true;
    readSettings();
}
- (void)windowDidResignKey:(NSNotification *)n
{
    window.focused = false;
    releaseInput();
}
@end

@interface VCSAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation VCSAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
    window.quitRequested = true; // Cmd+Q, the Dock's Quit: the UI decides
    window.needsRedraw = true;
    return NSTerminateCancel;
}
- (BOOL)applicationShouldHandleReopen:(NSApplication *)sender hasVisibleWindows:(BOOL)visible
{
    restoreAndRaise();
    return YES;
}
@end

namespace platform
{
    namespace
    {
        id g_appDelegate = nil;

        void makeMenu(const char *title)
        {
            NSString *name = [NSString stringWithUTF8String:title];
            NSMenu *bar = [NSMenu new];
            NSMenuItem *appItem = [NSMenuItem new];
            [bar addItem:appItem];
            NSMenu *app = [NSMenu new];
            [app addItemWithTitle:[@"Hide " stringByAppendingString:name] action:@selector(hide:) keyEquivalent:@"h"];
            NSMenuItem *others = [app addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
            [others setKeyEquivalentModifierMask:NSEventModifierFlagOption | NSEventModifierFlagCommand];
            [app addItem:[NSMenuItem separatorItem]];
            [app addItemWithTitle:[@"Quit " stringByAppendingString:name] action:@selector(terminate:) keyEquivalent:@"q"];
            [appItem setSubmenu:app];
            NSMenuItem *windowItem = [NSMenuItem new];
            [bar addItem:windowItem];
            NSMenu *win = [[NSMenu alloc] initWithTitle:@"Window"];
            [win addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
            [win addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
            [win addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
            [windowItem setSubmenu:win];
            [NSApp setMainMenu:bar];
            [NSApp setWindowsMenu:win];
        }

        // a timer that draws frames while macOS runs a loop of its own (open / save panels)
        NSTimer *modalTimer()
        {
            NSTimer *t = [NSTimer timerWithTimeInterval:1.0 / 30.0
                                                repeats:YES
                                                  block:^(NSTimer *) {
                                                    runModalFrame();
                                                  }];
            [[NSRunLoop currentRunLoop] addTimer:t forMode:NSModalPanelRunLoopMode];
            [[NSRunLoop currentRunLoop] addTimer:t forMode:NSEventTrackingRunLoopMode];
            return t;
        }

        NSArray<UTType *> *contentTypes(const std::vector<FileType> &types)
        {
            NSMutableArray<UTType *> *out = [NSMutableArray array];
            for (const FileType &t : types)
            {
                const std::string pattern = t.pattern ? t.pattern : "";
                if (pattern.rfind("*.", 0) != 0)
                    return nil; // "*": anything
                if (UTType *u = [UTType typeWithFilenameExtension:[NSString stringWithUTF8String:pattern.c_str() + 2]])
                    [out addObject:u];
            }
            return [out count] ? out : nil;
        }

        bool runPanel(NSSavePanel *panel, std::filesystem::path &out)
        {
            releaseInput();
            NSTimer *timer = modalTimer();
            const NSModalResponse r = [panel runModal];
            [timer invalidate];
            [g_window makeKeyWindow];
            if (r != NSModalResponseOK || ![panel URL])
                return false;
            out = std::filesystem::path([[panel URL] fileSystemRepresentation]);
            return true;
        }
    }

    void setChrome(const Chrome &chrome) { g_chrome = chrome; }

    bool init(const char *title, int logicalWidth, int logicalHeight, int minLogicalWidth, int minLogicalHeight,
              std::string &error)
    {
        now();
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        g_appDelegate = [VCSAppDelegate new];
        [NSApp setDelegate:g_appDelegate];
        makeMenu(title);
        [NSApp finishLaunching];

        const NSRect frame = NSMakeRect(0, 0, logicalWidth, logicalHeight);
        g_window = [[NSWindow alloc] initWithContentRect:frame
                                               styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                         NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable |
                                                         NSWindowStyleMaskFullSizeContentView
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
        if (!g_window)
        {
            error = "could not open a window";
            return false;
        }
        [g_window setReleasedWhenClosed:NO];
        [g_window setTitle:[NSString stringWithUTF8String:title]];
        [g_window setTitlebarAppearsTransparent:YES];
        [g_window setTitleVisibility:NSWindowTitleHidden];
        [g_window setContentMinSize:NSMakeSize(minLogicalWidth, minLogicalHeight)];
        [g_window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
        g_delegate = [VCSWindowDelegate new];
        [g_window setDelegate:g_delegate];
        g_view = [[VCSView alloc] initWithFrame:frame];
        [g_view setWantsLayer:YES];
        [g_view registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
        [g_window setContentView:g_view];
        [g_window makeFirstResponder:g_view];
        [g_window center];
        if (!g_layer)
        {
            error = "no Metal layer for the window";
            return false;
        }
        updateSize();
        readSettings();
        return true;
    }

    void show()
    {
        [g_window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        updateSize();
    }

    void shutdown()
    {
        if (g_window)
        {
            [g_window setDelegate:nil];
            [g_window orderOut:nil];
            [g_window close];
        }
        g_window = nil;
        g_view = nil;
        g_layer = nil;
        g_delegate = nil;
    }

    void *nativeHandle() { return (__bridge void *)g_layer; }

    void errorBox(const std::string &text)
    {
        std::fprintf(stderr, "VCS Launcher: %s\n", text.c_str());
        if (std::getenv("VCS_LAUNCHER_NO_DIALOGS"))
            return;
        @autoreleasepool
        {
            [NSApplication sharedApplication];
            NSAlert *alert = [NSAlert new];
            [alert setMessageText:@"VCS Launcher"];
            [alert setInformativeText:[NSString stringWithUTF8String:text.c_str()]];
            [alert setAlertStyle:NSAlertStyleCritical];
            [alert runModal];
        }
    }

    void pumpEvents()
    {
        @autoreleasepool
        {
            for (;;)
            {
                NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate distantPast]
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
                if (!e)
                    break;
                if ([e type] != NSEventTypeApplicationDefined)
                    [NSApp sendEvent:e];
            }
            [NSApp updateWindows];
        }
    }

    void waitForEvents(double timeoutSeconds)
    {
        @autoreleasepool
        {
            NSDate *until = timeoutSeconds < 0 ? [NSDate distantFuture] : [NSDate dateWithTimeIntervalSinceNow:timeoutSeconds];
            NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES];
            if (e && [e type] != NSEventTypeApplicationDefined)
                [NSApp sendEvent:e];
        }
        pumpEvents();
    }

    void wake()
    {
        // NSApp's queue takes events from any thread
        @autoreleasepool
        {
            NSEvent *e = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                            location:NSZeroPoint
                                       modifierFlags:0
                                           timestamp:0
                                        windowNumber:0
                                             context:nil
                                             subtype:0
                                               data1:0
                                               data2:0];
            [NSApp postEvent:e atStart:NO];
        }
    }

    void endFrame()
    {
        for (int i = 0; i < 3; ++i)
            input.pressed[i] = input.released[i] = false;
        input.clicks = 0;
        input.wheel = 0;
        input.mouseMoved = false;
        std::fill(std::begin(input.keyPressed), std::end(input.keyPressed), false);
        input.text.clear();
        input.dropped.clear();
    }

    void setModalFrame(void (*frame)()) { g_modalFrame = frame; }

    void setCursor(Cursor cursor)
    {
        if (cursor == g_cursor)
            return;
        g_cursor = cursor;
        if (input.mouseInside)
            [cursorFor(cursor) set];
    }

    void minimize() { [g_window miniaturize:nil]; }

    void toggleMaximize() { [g_window zoom:nil]; }

    void restoreAndRaise()
    {
        if ([g_window isMiniaturized])
            [g_window deminiaturize:nil];
        [g_window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }

    void setDarkFrame(bool dark)
    {
        [g_window setAppearance:[NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua]];
    }

    void releaseInput()
    {
        for (int i = 0; i < 3; ++i)
            input.down[i] = false;
        std::fill(std::begin(input.keyDown), std::end(input.keyDown), false);
        input.ctrl = input.shift = input.alt = false;
        window.needsRedraw = true;
    }

    double now()
    {
        static timespec start{};
        timespec t{};
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (start.tv_sec == 0 && start.tv_nsec == 0)
            start = t;
        return double(t.tv_sec - start.tv_sec) + double(t.tv_nsec - start.tv_nsec) * 1e-9;
    }

    void sleepFor(double seconds)
    {
        if (seconds <= 0)
            return;
        timespec t{time_t(seconds), long((seconds - std::floor(seconds)) * 1e9)};
        while (nanosleep(&t, &t) != 0 && errno == EINTR)
        {
        }
    }

    bool openFileDialog(std::filesystem::path &out, const char *title, const std::vector<FileType> &types)
    {
        @autoreleasepool
        {
            NSOpenPanel *panel = [NSOpenPanel openPanel];
            [panel setMessage:[NSString stringWithUTF8String:title]];
            [panel setCanChooseFiles:YES];
            [panel setCanChooseDirectories:NO];
            [panel setAllowsMultipleSelection:NO];
            if (NSArray<UTType *> *allowed = contentTypes(types))
                [panel setAllowedContentTypes:allowed];
            return runPanel(panel, out);
        }
    }

    bool saveFileDialog(std::filesystem::path &out, const char *title, const std::filesystem::path &suggested,
                        const std::vector<FileType> &types, const char *defaultExtension)
    {
        @autoreleasepool
        {
            NSSavePanel *panel = [NSSavePanel savePanel];
            [panel setMessage:[NSString stringWithUTF8String:title]];
            if (!suggested.empty())
            {
                if (suggested.has_parent_path())
                    [panel setDirectoryURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:suggested.parent_path().c_str()]]];
                [panel setNameFieldStringValue:[NSString stringWithUTF8String:suggested.filename().c_str()]];
            }
            if (NSArray<UTType *> *allowed = contentTypes(types))
                [panel setAllowedContentTypes:allowed];
            if (!runPanel(panel, out))
                return false;
            if (defaultExtension && *defaultExtension && !out.has_extension())
                out += std::string(".") + (defaultExtension[0] == '.' ? defaultExtension + 1 : defaultExtension);
            return true;
        }
    }

    bool openFolderDialog(std::filesystem::path &out, const char *title, const std::filesystem::path &start)
    {
        @autoreleasepool
        {
            NSOpenPanel *panel = [NSOpenPanel openPanel];
            [panel setMessage:[NSString stringWithUTF8String:title]];
            [panel setCanChooseFiles:NO];
            [panel setCanChooseDirectories:YES];
            [panel setCanCreateDirectories:YES];
            [panel setAllowsMultipleSelection:NO];
            if (!start.empty())
                [panel setDirectoryURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:start.c_str()]]];
            return runPanel(panel, out);
        }
    }
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN); // a tool that exits while we write to it is not our end
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.push_back(argv[i]);
    @autoreleasepool
    {
        return launcherMain(args);
    }
}
