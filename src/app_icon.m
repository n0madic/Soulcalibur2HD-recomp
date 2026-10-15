/*
 * The Dock icon on macOS. The binary is not an app bundle, so there is no icon file to find: the picture is compiled in
 * (sc2hd_icon.h, made from CMake's SC2HD_ICON_PNG: the neutral icon of assets/icon/ in the host, the game's own
 * ICON0.PNG in a personal-use monolith). On Windows the icon is a resource of the executable instead (sc2hd.rc.in).
 */
#import <AppKit/AppKit.h>

#include "sc2hd_icon.h"

/* Called by the toolkit's Metal backend (patches/0028). */
void sc2hd_set_app_icon(void)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        unsigned char* planes[1] = { (unsigned char*)sc2hd_icon_rgba };
        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:planes
                          pixelsWide:SC2HD_ICON_SIZE
                          pixelsHigh:SC2HD_ICON_SIZE
                       bitsPerSample:8
                     samplesPerPixel:4
                            hasAlpha:YES
                            isPlanar:NO
                      colorSpaceName:NSDeviceRGBColorSpace
                         bytesPerRow:SC2HD_ICON_SIZE * 4
                        bitsPerPixel:32];
        NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(SC2HD_ICON_SIZE, SC2HD_ICON_SIZE)];
        [image addRepresentation:rep];   /* the representation copies nothing: it keeps pointing at the static array */
        [NSApp setApplicationIconImage:image];
    }
}
