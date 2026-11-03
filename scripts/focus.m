// focus <pid> — bring one process's windows to the front, by pid.
//
// ⚠️ WHY THIS EXISTS. metal_backend.mm:1692 is explicit: "The engine's input
// frame refuses to poll anything until it believes it is the foreground
// window", and gfx_pump_once() pushes WM_ACTIVATEAPP(0) the moment
// [g_window isKeyWindow] goes false. So if ANY other application is frontmost
// — a browser the user clicked into, anything — every keystroke sent with
// scripts/sendkey is delivered to the process, translated, queued, and then
// ignored by the guest. The symptom is indistinguishable from dead keyboard
// input, from a wrongly-timed keypress, and from a hung frontend.
//
// The runtime already calls activateIgnoringOtherApps:YES once, when it creates
// the window. Nothing re-asserts it later, so focus is only correct if nobody
// touches the Mac for the length of a fifteen-minute run.
//
// NSRunningApplication rather than System Events: `tell application "System
// Events" to set frontmost of process "boot"` returns without error and does
// NOT change the frontmost application (measured — it reported Brave Browser
// still frontmost immediately afterwards), because the game is not an
// accessibility-scriptable app bundle.
#import <AppKit/AppKit.h>
int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: focus <pid>\n"); return 2; }
    pid_t pid = (pid_t)atoi(argv[1]);
    NSRunningApplication* a =
        [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
    if (!a) { fprintf(stderr, "focus: no running application for pid %d\n", pid); return 1; }
    BOOL ok = [a activateWithOptions:NSApplicationActivateIgnoringOtherApps |
                                     NSApplicationActivateAllWindows];
    // Say what actually happened, and verify it rather than trusting the return.
    usleep(300000);
    NSRunningApplication* front = [[NSWorkspace sharedWorkspace] frontmostApplication];
    fprintf(stderr, "focus: activate=%s frontmost is now pid %d (%s)\n",
            ok ? "yes" : "no", [front processIdentifier],
            [[front localizedName] UTF8String] ?: "?");
    return ([front processIdentifier] == pid) ? 0 : 1;
}
