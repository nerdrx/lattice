#include "midi_in.h"
#include "engine.h"

#include <alsa/asoundlib.h>
#include <poll.h>

#include <algorithm>
#include <vector>

namespace lat {

// Translates one sequencer event into the engine's 3-byte wire form. Anything
// we do not understand (clock, sysex, port subscriptions) is dropped: the
// engine hands raw bytes straight to plugins, so forwarding half-understood
// events would only confuse them.
static bool toWire(const snd_seq_event_t& ev, MidiMsg& m) {
    const auto chan = [&](unsigned c) { return (u8)(c & 0x0F); };
    switch (ev.type) {
    case SND_SEQ_EVENT_NOTEON:
        m.status = (u8)(0x90 | chan(ev.data.note.channel));
        m.d1 = (u8)(ev.data.note.note & 0x7F);
        m.d2 = (u8)(ev.data.note.velocity & 0x7F);
        // Note-on at velocity 0 is a note-off on the wire. Normalising here
        // means every instrument downstream only has to handle one shape.
        if (m.d2 == 0) m.status = (u8)(0x80 | chan(ev.data.note.channel));
        break;
    case SND_SEQ_EVENT_NOTEOFF:
        m.status = (u8)(0x80 | chan(ev.data.note.channel));
        m.d1 = (u8)(ev.data.note.note & 0x7F);
        m.d2 = (u8)(ev.data.note.velocity & 0x7F);
        break;
    case SND_SEQ_EVENT_KEYPRESS:
        m.status = (u8)(0xA0 | chan(ev.data.note.channel));
        m.d1 = (u8)(ev.data.note.note & 0x7F);
        m.d2 = (u8)(ev.data.note.velocity & 0x7F);
        break;
    case SND_SEQ_EVENT_CONTROLLER:
        m.status = (u8)(0xB0 | chan(ev.data.control.channel));
        m.d1 = (u8)(ev.data.control.param & 0x7F);
        m.d2 = (u8)(ev.data.control.value & 0x7F);
        break;
    case SND_SEQ_EVENT_PGMCHANGE:
        m.status = (u8)(0xC0 | chan(ev.data.control.channel));
        m.d1 = (u8)(ev.data.control.value & 0x7F);
        m.d2 = 0;
        break;
    case SND_SEQ_EVENT_CHANPRESS:
        m.status = (u8)(0xD0 | chan(ev.data.control.channel));
        m.d1 = (u8)(ev.data.control.value & 0x7F);
        m.d2 = 0;
        break;
    case SND_SEQ_EVENT_PITCHBEND: {
        // ALSA centres pitch bend on 0 and signs it; the wire format is a
        // 14-bit unsigned value, LSB first, centred on 8192.
        const int v = clampv((int)ev.data.control.value + 8192, 0, 16383);
        m.status = (u8)(0xE0 | chan(ev.data.control.channel));
        m.d1 = (u8)(v & 0x7F);
        m.d2 = (u8)((v >> 7) & 0x7F);
        break;
    }
    default:
        return false;
    }
    // The sequencer timestamps in real time, not in the audio callback's frame
    // clock, and the two are not phase-locked. Rather than guess an offset that
    // could land in the past, everything is stamped at the start of the block
    // the engine happens to be in; jitter is bounded by one buffer.
    m.frame = 0;
    return true;
}

bool MidiInput::start(Engine& e) {
    if (running_.load(std::memory_order_relaxed)) return true;

    snd_seq_t* seq = nullptr;
    const int err = snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0);
    if (err < 0 || !seq) {
        // Perfectly normal in containers and on kernels without snd-seq, so
        // this is a warning and not a failure the caller has to handle.
        LOGW("MIDI in: no ALSA sequencer (%s)", snd_strerror(err));
        return false;
    }
    snd_seq_set_client_name(seq, "Lattice");

    const int port = snd_seq_create_simple_port(
        seq, "in",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (port < 0) {
        LOGW("MIDI in: cannot create port (%s)", snd_strerror(port));
        snd_seq_close(seq);
        return false;
    }

    seq_     = seq;
    engine_  = &e;
    client_  = snd_seq_client_id(seq);
    port_    = port;
    received_.store(0, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread(&MidiInput::run, this);

    // Nothing is auto-connected on purpose; tell the user where to point their
    // controller instead of grabbing every keyboard on the system.
    LOGI("MIDI in: ALSA sequencer \"Lattice:in\" at %d:%d  (aconnect <source> %d:%d)",
         client_, port_, client_, port_);
    return true;
}

void MidiInput::stop() {
    if (!seq_) return;
    running_.store(false, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
    snd_seq_close((snd_seq_t*)seq_);
    seq_    = nullptr;
    engine_ = nullptr;
    client_ = -1;
    port_   = -1;
}

void MidiInput::run() {
    auto* seq = (snd_seq_t*)seq_;

    // snd_seq_event_input() blocks with no timeout and no cancellation point,
    // which would make stop() hang until the next note arrived. Polling the
    // sequencer's own descriptors with a short timeout keeps the read path
    // blocking (no busy loop) while still checking the run flag ~10x a second.
    const int npfd = std::max(1, snd_seq_poll_descriptors_count(seq, POLLIN));
    std::vector<pollfd> pfd((size_t)npfd);

    while (running_.load(std::memory_order_relaxed)) {
        snd_seq_poll_descriptors(seq, pfd.data(), (unsigned)npfd, POLLIN);
        const int ready = ::poll(pfd.data(), (nfds_t)npfd, 100);
        if (ready <= 0) continue;

        // Drain everything the sequencer has queued before going back to poll:
        // a controller sweep can deliver dozens of events per wakeup.
        for (;;) {
            snd_seq_event_t* ev = nullptr;
            if (snd_seq_event_input(seq, &ev) < 0 || !ev) break;
            MidiMsg m;
            if (toWire(*ev, m) && engine_ && engine_->pushMidi(m))
                received_.fetch_add(1, std::memory_order_relaxed);
            if (snd_seq_event_input_pending(seq, 0) <= 0) break;
        }
    }
}

} // namespace lat
