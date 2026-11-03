// Print the CoreGraphics window id of the BO2 window, so screencapture -l can
// grab ONLY that window. A full-screen grab would photograph whatever else is in
// front, which is the user's business and not ours.
//
// Two traps paid for here:
//  * kCGWindowListOptionOnScreenOnly MISSES the window whenever it is not on the
//    active Space or is fully occluded — and then shot.sh silently fails at the
//    exact moment you want a picture. Use ...OptionAll; screencapture -l still
//    captures an off-screen window's own surface.
//  * The process owns several menubar-strip windows (height 30). Taking the
//    first match grabs one of those, so choose the LARGEST by area.
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
int main(void) {
    CFArrayRef wl = CGWindowListCopyWindowInfo(kCGWindowListOptionAll, kCGNullWindowID);
    int best = 0; long bestArea = 0;
    for (NSDictionary* w in (__bridge NSArray*)wl) {
        NSString* owner = w[(id)kCGWindowOwnerName];
        NSDictionary* b = w[(id)kCGWindowBounds];
        if (![owner isEqualToString:@"boot"]) continue;
        if ([b[@"Height"] intValue] < 100) continue;      // menubar strips
        long area = (long)[b[@"Width"] intValue] * [b[@"Height"] intValue];
        if (area > bestArea) { bestArea = area; best = [w[(id)kCGWindowNumber] intValue]; }
    }
    if (!best) return 1;
    printf("%d\n", best);
    return 0;
}
