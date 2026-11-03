/*
 * toy.c — the "recompiled" side of the harness toy proof-of-life.
 *
 * This is the hand-written native (arm64) equivalent of the synthetic x86-32
 * function executed by OrigRunner. In the real harness this file would be
 * the output of the lift->AOT pipeline (Remill-lifted LLVM IR compiled by
 * clang -target arm64-apple-macos). Here it stands in for that output to
 * prove the differential-testing methodology end-to-end on a trivial target.
 *
 * x86-32 original (assembled bytes, see run_toy.py):
 *   shl  eax, 3        ; eax = a << 3
 *   imul ebx, ebx, 7   ; ebx = b * 7   (clobbers ebx)
 *   add  eax, ebx      ; eax = (a<<3) + b*7
 *   add  eax, 1        ; eax = (a<<3) + (b*7) + 1
 *   ret
 */
int toy(int a, int b) {
    int t = a << 3;       /* shl eax, 3 */
    int u = b * 7;        /* imul ebx, ebx, 7 */
    return t + u + 1;     /* add eax, ebx ; add eax, 1 */
}
