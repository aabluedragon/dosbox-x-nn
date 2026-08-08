// iOS shim for dosbox-x: unpack the bundled DOS environment into Documents,
// generate a conf with absolute container paths, then hand control to
// dosbox-x's SDL_main via SDL's UIKit runner.
#import <UIKit/UIKit.h>
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_main.h>
#include <stdio.h>
#include <string.h>

// dosbox-x's main(), renamed by SDL_main.h when it was compiled for iOS.
extern int SDL_main(int argc, char *argv[]);

static NSString *documentsDir(void) {
    return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
                                               NSUserDomainMask, YES)[0];
}

// dosbox-x prints startup errors to stdout/stderr, which iOS discards;
// keep them in files so failures are diagnosable.
static void redirectNativeOutput(void) {
    NSString *docs = documentsDir();
    freopen([docs stringByAppendingPathComponent:@"stdout.txt"].fileSystemRepresentation, "w", stdout);
    freopen([docs stringByAppendingPathComponent:@"stderr.txt"].fileSystemRepresentation, "w", stderr);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

// Copies the bundled DOS environment out of the app bundle on first launch.
// The marker file's presence means the copy is complete; bump it to force a
// re-extract on upgrade.
static NSString *prepareDosEnvironment(void) {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *dosDir = [documentsDir() stringByAppendingPathComponent:@"dos"];
    NSString *marker = [dosDir stringByAppendingPathComponent:@".unpacked-v1"];

    if (![fm fileExistsAtPath:marker]) {
        NSString *src = [[[NSBundle mainBundle] resourcePath]
                            stringByAppendingPathComponent:@"dos"];
        [fm removeItemAtPath:dosDir error:nil];
        NSError *err = nil;
        if (![fm copyItemAtPath:src toPath:dosDir error:&err]) {
            fprintf(stderr, "asset unpack failed: %s\n",
                    err.localizedDescription.UTF8String);
            abort();
        }
        [[NSData data] writeToFile:marker atomically:YES];
    }

    // The container path changes across installs, so the conf is written
    // fresh on every launch with absolute paths.
    NSString *conf = [NSString stringWithFormat:
        @"[sdl]\n"
        @"fullscreen = true\n"
        @"autolock   = false\n"
        @"output     = surface\n"
        @"showmenu   = false\n"
        @"\n"
        @"[dosbox]\n"
        @"machine  = svga_s3\n"
        @"memsize  = 32\n"
        @"captures = %@/captures\n"
        @"\n"
        @"[render]\n"
        @"aspect = true\n"
        @"\n"
        @"[cpu]\n"
        @"core    = normal\n"
        @"cputype = auto\n"
        @"cycles  = auto\n"
        @"\n"
        @"[sblaster]\n"
        @"sbtype = sb16\n"
        @"sbbase = 220\n"
        @"irq    = 7\n"
        @"dma    = 1\n"
        @"hdma   = 5\n"
        @"\n"
        @"[mixer]\n"
        @"rate      = 44100\n"
        @"blocksize = 1024\n"
        @"\n"
        @"[autoexec]\n"
        @"mount c \"%@\"\n"
        @"c:\n"
        @"cd DUKE3D\n"
        @"DUKE3D.EXE\n",
        dosDir, dosDir];
    NSString *confPath = [dosDir stringByAppendingPathComponent:@"dosbox-ios.conf"];
    [conf writeToFile:confPath atomically:YES
             encoding:NSUTF8StringEncoding error:nil];
    return confPath;
}

int main(int argc, char *argv[]) {
    @autoreleasepool {
        // SDL decides the allowed orientations from this hint; without it
        // the window comes up portrait and the game renders sideways.
        setenv("SDL_ORIENTATIONS", "LandscapeLeft LandscapeRight", 1);
        redirectNativeOutput();
        NSString *confPath = prepareDosEnvironment();
        static char confArg[1024];
        strlcpy(confArg, confPath.fileSystemRepresentation, sizeof(confArg));
        static char name[] = "dosbox-x", opt[] = "-conf";
        char *args[] = { name, opt, confArg, NULL };
        return SDL_UIKitRunApp(3, args, SDL_main);
    }
}
