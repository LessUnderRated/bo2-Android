// sendkey <pid> <macVirtualKeycode> [holdMs] — post one key down+up to ONE process.
// CGEventPostToPid targets only that process: it does not steal focus and it
// cannot type into whatever the user has in front, which a global event post
// (or an AppleScript "keystroke") absolutely would.
//
// ⚠️ THE HOLD MUST OUTLAST AN ENGINE FRAME. It was a fixed 60 ms, which is fine at
// the ~18 fps of a no-video run (55 ms/frame) and silently useless in the
// video-enabled configuration: the title bar of that run reads "5.8 fps engine",
// i.e. 172 ms per frame, so both the down and the up landed between two input
// polls and the game never observed the key as pressed. Three validates in a row
// did nothing on a correctly focused "[ NEW STORY ]", and it read as the game
// refusing to start a mission. Caller-settable, so the runner can scale it with
// the frame rate it expects.
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#include <unistd.h>
int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: sendkey <pid> <keycode> [holdMs]\n"); return 2; }
    pid_t pid = (pid_t)atoi(argv[1]);
    CGKeyCode kc = (CGKeyCode)strtol(argv[2], NULL, 0);
    long hold = (argc > 3) ? strtol(argv[3], NULL, 0) : 60;
    if (hold < 1) hold = 1;
    if (hold > 5000) hold = 5000;
    CGEventRef d = CGEventCreateKeyboardEvent(NULL, kc, true);
    CGEventRef u = CGEventCreateKeyboardEvent(NULL, kc, false);
    CGEventPostToPid(pid, d); usleep((useconds_t)(hold * 1000));
    CGEventPostToPid(pid, u);
    CFRelease(d); CFRelease(u);
    return 0;
}
