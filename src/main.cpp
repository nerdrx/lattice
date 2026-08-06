// NxTakt — a native, session-first DAW for Linux.
#include "ui/app.h"
#include "audio/backend.h"
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstdlib>

#if defined(__x86_64__) || defined(__i386__)
#include <pmmintrin.h>
#include <xmmintrin.h>
#endif

static void usage() {
    std::printf(
        "NxTakt — session-first DAW\n"
        "\n"
        "  nxtakt [project.lattice]\n"
        "\n"
        "Environment:\n"
        "  NXTAKT_BACKEND=wayland|x11    force a window backend\n"
        "  NXTAKT_AUDIO=jack|alsa        force an audio backend\n"
        "  NXTAKT_SCALE=1.5              override UI scale\n"
        "  (the pre-rename LATTICE_* spellings are still read; NXTAKT_* wins)\n"
        "\n"
        "Keys:\n"
        "  Space          play / stop            Esc      stop all clips\n"
        "  Tab            Session / Arrangement  Enter    launch selected clip\n"
        "  Arrows         move selection         Del      clear selected clip\n"
        "  M              metronome              Ctrl+S   save\n"
        "  Ctrl+B         browser                Ctrl+D   clip detail\n"
        "  Ctrl+T         add track              Ctrl+Enter add scene\n"
        "  Ctrl+Z         undo                   Ctrl+Shift+Z / Ctrl+Y  redo\n"
        "                 (edits only: the transport, the record button and a\n"
        "                  take in flight are outside it -- an undo while\n"
        "                  recording cancels the take)\n"
        "  Ctrl+Shift+K   computer MIDI keyboard (plays the armed track)\n"
        "                 FL layout, by key position (any keyboard layout):\n"
        "                            Z X C V B N M = lower octave white keys,\n"
        "                            S D   G H J   = its black keys,\n"
        "                 Q W E R T Y U + I O P    = the two octaves above,\n"
        "                 2 3   5 6 7   9 0        = their black keys,\n"
        "                 PgUp / PgDn octave, velocity next to the KBD chip\n"
        "\n"
        "Recording (the round button arms the intent, like Live's session record):\n"
        "  empty slot     click starts a take on an armed track, click again stops\n"
        "  MIDI clip      click overdubs another pass into it, click again stops\n"
        "\n"
        "Piano roll (CLIP tab, MIDI clips):\n"
        "  Click          add / select note      Double-click  add / delete\n"
        "  Drag           move, right edge sizes Right-click   delete note\n"
        "  Wheel          scroll                 Shift+wheel   scroll time\n"
        "  Ctrl+wheel     zoom time about the cursor\n"
        "  Arrows         nudge the selected note (grid step / semitone)\n"
        "  Shift+Up/Down  nudge by an octave      Del      delete the note\n"
        "  Esc            deselect the note (again: stop all clips)\n"
        "  Ctrl+U         double the loop and duplicate its notes\n");
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(); return 0; }
    }

    std::setlocale(LC_ALL, "");
    // LC_NUMERIC must stay in the C locale. The project format is written and
    // read with printf/strtod, so under a comma-decimal locale (de_DE, fr_FR,
    // ...) every "0.85" in a saved set would parse as 0 and every number we
    // wrote would be unreadable anywhere else.
    std::setlocale(LC_NUMERIC, "C");

    // A dying JACK/ALSA peer must not take the process with it.
    std::signal(SIGPIPE, SIG_IGN);

    // Before anything can touch ALSA -- and plugins do, whether or not ALSA is
    // our audio backend. libasound prints its own diagnostics straight to
    // stderr, in red, saying "error", and on any machine with a partial
    // /usr/share/alsa config an instrument that opens a device of its own emits
    // dozens of them. They arrive with no attribution, so they read as this
    // program crashing. Routed through our logger they are labelled, printed
    // once each, and obviously not ours.
    lat::alsaInstallLogHandler();

#if defined(__x86_64__) || defined(__i386__)
    // Denormals in feedback tails cost orders of magnitude on the audio thread.
    // These are per-thread, but the audio thread is created after this point.
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif

    // Heap-allocated: Engine carries per-track fx scratch (~2 MB), which has
    // no business on main's stack.
    auto app = std::make_unique<lat::App>();
    if (!app->init(argc, argv)) {
        std::fprintf(stderr, "nxtakt: failed to start\n");
        return 1;
    }
    app->run();
    app->shutdown();
    return 0;
}
