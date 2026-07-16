// PARRecorder — tiny launcher that gives the YT-Streamer daemon a real, signed,
// promptable macOS bundle identity (com.par.ytstreamer) as its TCC "responsible"
// process, so the camera consent prompt can appear and the grant persists —
// independent of who launches it (launchd, SSH, reboot). See the
// com.par.ytstreamer LaunchAgent and the yt-streamer-camera-tcc memory.
//
// WHY THIS EXISTS: the daemon runs as org.python.python (Python.app), which has no
// NSCameraUsageDescription and isn't camera-granted, so under launchd its capture
// session starts but streams ZERO frames (silent empty recordings). It worked
// before ONLY when launched from Terminal (an already-granted bundle it inherited
// from). This launcher is a signed bundle (com.par.ytstreamer) WITH a camera usage
// string; the one-time consent prompt attributes to it, and the python daemon —
// spawned as its child — inherits Authorized as its responsible process.
//
// CRITICAL: do NOT exec() the payload. exec() replaces this PID's code identity,
// so TCC would see caffeinate/python instead of com.par.ytstreamer. Instead FORK
// the payload and stay alive as the signed identity — the long-lived responsible
// ancestor the grant attaches to. SIGTERM/SIGINT are forwarded so
// `launchctl bootout`/stop tears the daemon down cleanly.
//
// REBUILD CAVEAT: recompiling + re-signing changes the ad-hoc cdhash, which voids
// the existing camera grant — after any rebuild, the consent prompt must be
// approved once more (Screen Sharing → Allow). See launchagent README.
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

static pid_t g_child = 0;

static void forward(int sig) {
    if (g_child > 0) kill(g_child, sig);
}

int main(void) {
    if (chdir("/Users/admin/P.A.R./YT-Streamer") != 0) {
        perror("chdir");
        return 1;
    }
    signal(SIGTERM, forward);
    signal(SIGINT, forward);

    g_child = fork();
    if (g_child < 0) {
        perror("fork");
        return 1;
    }
    if (g_child == 0) {
        char *const args[] = {
            "/usr/bin/caffeinate", "-i",
            "/Users/admin/P.A.R./YT-Streamer/venv/bin/python",
            "/Users/admin/P.A.R./YT-Streamer/watchdog.py",
            NULL
        };
        execv("/usr/bin/caffeinate", args);
        perror("execv");   // child only reaches here if exec fails
        _exit(1);
    }

    // Parent: keep the com.par.ytstreamer identity alive and reap the child.
    int status = 0;
    while (waitpid(g_child, &status, 0) < 0 && errno == EINTR) { }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
