// Devices: chain-owner addressing, add / remove / publish chains, the plugin
// scan, and the whole DEVICES tab (browser + strip + param knobs). Moved
// verbatim from app.cpp.
//
#include "app.h"
#include "app_internal.h"
#include "pianoroll.h"
#include "../core/project.h"
#include "../gfx/gl.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <new>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

namespace lat {

// ---------------------------------------------------------------------------
// device chains
//
// See the lifecycle comment in app.h: the GUI owns every RtChain and every
// PluginInstance, the audio thread only borrows them, and the handshake below
// is the only path on which anything is freed while audio runs.
// ---------------------------------------------------------------------------

// Where one owner's chain lives. The three cases differ in nothing else, which
// is the whole point: past this function no code below knows what a return is.
App::ChainOwner App::chainOwner(int owner) {
    ChainOwner co;
    if (owner == kOwnMaster) {
        co.devices   = &ses_.masterDevices;
        co.saved     = &ses_.masterSavedDevices;
        co.published = &publishedMaster_;
        co.cmd       = Cmd::SetMasterChain;
        co.addr      = -1;                      // as the retirement event says it
    } else if (ownIsReturn(owner)) {
        ReturnModel& rm = ses_.returns[owner - kOwnReturn0];
        co.devices   = &rm.devices;
        co.saved     = &rm.savedDevices;
        co.published = &publishedReturn_[owner - kOwnReturn0];
        co.cmd       = Cmd::SetReturnChain;
        co.addr      = owner - kOwnReturn0;
    } else if (ownIsTrack(owner)) {
        co.published = &published_[owner];
        co.cmd       = Cmd::SetChain;
        co.addr      = owner;
        // A published slot outlives the track model: a set that shrank leaves
        // the engine running a chain for an index the session no longer has.
        if (owner < (int)ses_.tracks.size()) {
            co.devices = &ses_.tracks[owner].devices;
            co.saved   = &ses_.tracks[owner].savedDevices;
        }
    }
    return co;
}

std::string App::ownerName(int owner) const {
    if (owner == kOwnMaster) return "Master";
    if (ownIsReturn(owner)) return std::string("Return ") + kReturnLetter[owner - kOwnReturn0];
    if (owner >= 0 && owner < (int)ses_.tracks.size()) return ses_.tracks[owner].name;
    char buf[32];
    snprintf(buf, sizeof buf, "track %d", owner);
    return buf;
}

std::vector<int> App::modelOwners() const {
    std::vector<int> v;
    v.reserve(ses_.tracks.size() + kMaxReturns + 1);
    for (int t = 0; t < (int)ses_.tracks.size(); ++t) v.push_back(t);
    for (int i = 0; i < kMaxReturns; ++i) v.push_back(ownReturn(i));
    v.push_back(kOwnMaster);
    return v;
}

void App::publishChain(int owner) {
    ChainOwner co = chainOwner(owner);
    if (!co.valid() || !co.devices) return;

    RtChain* chain = new RtChain();
    int n = 0;
    for (const DeviceModel& d : *co.devices) {
        if (!d.inst) continue;
        if (n >= kMaxChainFx) {
            LOGW("%s has more than %d devices - the extras will not sound",
                 ownerName(owner).c_str(), kMaxChainFx);
            break;
        }
        // Bypassed devices stay in the chain: the instance itself short-circuits
        // in process(), which keeps the chain stable across a bypass toggle.
        chain->fx[n++] = d.inst.get();
    }
    chain->count = n;

    Command c;
    c.type = co.cmd;
    c.a = co.addr;
    c.p = chain;
    if (!engine_.pushCommand(c)) {
        // The ring is full, so the engine never saw this chain. It is still
        // solely ours, and the previously published one is still live: drop the
        // new one and leave every piece of state exactly as it was.
        LOGW("command ring full - chain for %s not published", ownerName(owner).c_str());
        delete chain;
        return;
    }

    if (*co.published) retiring_.push_back(RetiredChain{*co.published, {}});
    *co.published = chain;
}

void App::addDevice(int owner, const PluginDesc& d) {
    ChainOwner co = chainOwner(owner);
    if (!co.valid() || !co.devices) return;
    std::vector<DeviceModel>& devices = *co.devices;
    if ((int)devices.size() >= kMaxChainFx) {
        status_ = "Chain is full";
        return;
    }

    // instantiate() already calls prepare() on the instance (see the tail of
    // instantiateLV2/instantiateCLAP), so a non-null return is ready to run.
    std::unique_ptr<PluginInstance> inst =
        registry_.instantiate(d, engine_.sampleRate(), kMaxBlock);
    if (!inst) {
        status_ = "Could not load " + d.name;
        return;
    }

    DeviceModel dm;
    dm.uid = ses_.newUid();
    dm.desc = d;
    dm.inst = std::move(inst);
    devices.push_back(std::move(dm));

    const RtChain* before = *co.published;
    publishChain(owner);
    if (*co.published == before) {
        // Publish failed. The engine never referenced this instance, so it is
        // safe to destroy right here and leave the model matching the engine.
        devices.pop_back();
        status_ = "Engine busy - device not added";
        return;
    }
    selDevice_ = (int)devices.size() - 1;
    paramScroll_ = 0.f;
    status_ = "Added " + d.name;
}

void App::removeDevice(int owner, int idx) {
    ChainOwner co = chainOwner(owner);
    if (!co.valid() || !co.devices) return;
    std::vector<DeviceModel>& devices = *co.devices;
    if (idx < 0 || idx >= (int)devices.size()) return;

    // Move the instance out of the model rather than letting erase() destroy
    // it: the audio thread is still running the *outgoing* chain, which points
    // straight at it. It may only die once that chain comes back to us.
    DeviceModel dead = std::move(devices[idx]);
    devices.erase(devices.begin() + idx);

    const RtChain* outgoing = *co.published;
    publishChain(owner);

    if (*co.published == outgoing) {
        // Publish failed; the engine still runs the old chain, so the device
        // has to go back where it was or the model would lie about what sounds.
        devices.insert(devices.begin() + idx, std::move(dead));
        status_ = "Engine busy - device not removed";
        return;
    }
    if (outgoing) {
        // publishChain() just appended the entry for `outgoing`; the instance
        // rides along in it and is freed when Ev::ChainRetired arrives.
        retiring_.back().dying.push_back(std::move(dead.inst));
    }
    // Otherwise nothing was ever published, so nothing borrowed the instance
    // and it is freed as `dead` goes out of scope.

    if (devices.empty())                        selDevice_ = -1;
    else if (selDevice_ >= (int)devices.size()) selDevice_ = (int)devices.size() - 1;
    paramScroll_ = 0.f;
    status_ = "Removed " + dead.desc.name;
}

void App::ensurePluginScan() {
    if (registryScanned_) return;
    // lilv walks every bundle on the system and a CLAP scan dlopens each
    // binary, which costs the better part of a second. Deferring it to the
    // first time the DEVICES tab opens keeps startup snappy for anyone who
    // never touches a plugin.
    status_ = "Scanning plugins...";
    registry_.scan();
    registryScanned_ = true;
    char buf[48];
    snprintf(buf, sizeof buf, "%zu plugins", registry_.plugins().size());
    status_ = buf;
}


// ---------------------------------------------------------------------------
// device view: plugin browser on the left, the selected track's chain right
// ---------------------------------------------------------------------------

void App::drawDeviceDetail(const Rect& r) {
    const f32 s = win_.dpiScale();
    // The scan is lazy, and the tab can also be reached by restoring a session
    // with the tab already active, so make sure it has happened.
    ensurePluginScan();

    const f32 listW = 236 * s;
    Rect list{r.x, r.y, listW, r.h};
    Rect strip{list.right() + 1 * s, r.y, r.right() - list.right() - 1 * s, r.h};
    drawPluginBrowser(list);
    rend_.rect({list.right(), r.y, 1 * s, r.h}, pal::divider);
    drawDeviceStrip(strip);
}

void App::drawPluginBrowser(const Rect& r) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    rend_.rect(r, pal::panelAlt);

    // --- filter ---
    const u64 fid = uiId(10, 0);
    Rect filter{r.x + 6 * s, r.y + 5 * s, r.w - 12 * s, 17 * s};
    ui_.textField(fid, filter, &pluginFilter_, pal::appBg, pal::text, Align::Left, false);
    // textField only writes back on commit, but a filter has to narrow as you
    // type, so read the live edit buffer while this field owns the caret.
    const std::string* live = ui_.liveText(fid);
    const std::string& query = live ? *live : pluginFilter_;
    if (query.empty())
        rend_.textIn(fSmall_, filter, "filter plugins", pal::textFaint, Align::Left, 5 * s);

    // --- filtered index, rebuilt each frame: a few hundred string compares ---
    static std::vector<int> shown;                  // reused to avoid churn
    shown.clear();
    const std::vector<PluginDesc>& all = registry_.plugins();
    for (int i = 0; i < (int)all.size(); ++i)
        if (icontains(all[i].name, query)) shown.push_back(i);

    const f32 rowH = 17 * s;
    Rect listR{r.x, filter.bottom() + 4 * s, r.w, r.bottom() - filter.bottom() - 4 * s};
    rend_.pushClip(listR);
    rend_.rect(listR, pal::appBg.scale(1.05f));

    if (ui_.setHot(uiId(10, 1), listR) && in.wheel != 0.f) {
        pluginScroll_ -= in.wheel * rowH * 3.f;
    }
    const f32 maxScroll = std::max(0.f, shown.size() * rowH - listR.h);
    pluginScroll_ = clampv(pluginScroll_, 0.f, maxScroll);

    if (shown.empty()) {
        rend_.textIn(fSmall_, listR, all.empty() ? "no plugins found" : "no match",
                     pal::textFaint, Align::Center);
    }

    f32 y = listR.y - pluginScroll_;
    for (size_t k = 0; k < shown.size(); ++k) {
        Rect row{listR.x, y, listR.w, rowH};
        y += rowH;
        if (row.bottom() < listR.y || row.y > listR.bottom()) continue;

        const int pi = shown[k];
        const PluginDesc& d = all[pi];
        const u64 id = uiId(10, 100 + pi);
        const bool hot = ui_.setHot(id, row) && ui_.isHot(id);
        if (pi == pluginSel_) rend_.rect(row, pal::gridBg);
        else if (hot)         rend_.rect(row, pal::slotHover);
        if (hot) ui_.cursor = Cursor::Hand;

        Rect tag{row.right() - 34 * s, row.cy() - 6 * s, 28 * s, 12 * s};
        rend_.roundRect(tag, 2 * s, pal::panel);
        rend_.textIn(fSmall_, tag, formatName(d.format), pal::textFaint, Align::Center, 0);

        Rect vendor{tag.x - 74 * s, row.y, 70 * s, row.h};
        if (!d.vendor.empty()) {
            rend_.pushClip(vendor);
            rend_.textIn(fSmall_, vendor, d.vendor.c_str(), pal::textDim, Align::Right, 0);
            rend_.popClip();
        }

        Rect name{row.x + 8 * s, row.y, vendor.x - row.x - 12 * s, row.h};
        rend_.pushClip(name);
        rend_.textIn(fBody_, name, d.name.c_str(), hot || pi == pluginSel_ ? pal::text : pal::textDim,
                     Align::Left, 0);
        rend_.popClip();

        if (hot && in.pressed[0]) pluginSel_ = pi;
        // Double-click loads, matching how the file browser drops a sample.
        // The entry is taken here rather than inside addDevice, which
        // init() also calls through the NXTAKT_DEBUG_ADDFX hook: nothing that
        // happens while the app is starting up belongs in the history.
        if (hot && in.dblClick) {
            undoPoint("add device");
            addDevice(devOwner_, d);
        }
    }
    rend_.popClip();
}

void App::drawDeviceStrip(const Rect& r) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    rend_.rect(r, pal::panel);

    // The chain being edited belongs to a track, a return or the master; past
    // this point the only difference is the colour of the identity chip.
    ChainOwner co = chainOwner(devOwner_);
    if (!co.devices) {                       // the target went away under us
        devOwner_ = selTrack_;
        co = chainOwner(devOwner_);
        if (!co.devices) return;             // nothing is clipped yet
    }
    std::vector<DeviceModel>& devices = *co.devices;
    const Col tc = ownIsTrack(devOwner_)
                 ? pal::clipColors[ses_.tracks[devOwner_].colorIdx % pal::clipColorCount]
                 : (ownIsReturn(devOwner_) ? pal::soloBlue : pal::accent);

    Rect head{r.x, r.y, r.w, 16 * s};
    rend_.rect(head, pal::panelAlt);
    rend_.rect({head.x, head.y, 4 * s, head.h}, tc);       // owner identity chip
    rend_.textIn(fBold_, {head.x + 10 * s, head.y, 220 * s, head.h},
                 ownerName(devOwner_).c_str(), pal::text, Align::Left, 0);
    rend_.textIn(fSmall_, head, "double-click a plugin to add it to this chain",
                 pal::textFaint, Align::Right, 8 * s);

    Rect area{r.x, head.bottom(), r.w, r.bottom() - head.bottom()};
    rend_.pushClip(area);

    // Keep the selection honest: the target can be switched under it, and a
    // device can have been removed since the last frame.
    if (devices.empty()) selDevice_ = -1;
    else selDevice_ = clampv(selDevice_ < 0 ? 0 : selDevice_, 0, (int)devices.size() - 1);

    if (devices.empty()) {
        char msg[80];
        snprintf(msg, sizeof msg, "No devices on %s", ownerName(devOwner_).c_str());
        rend_.textIn(fBody_, area, msg, pal::textFaint, Align::Center);
        rend_.popClip();
        return;
    }

    const f32 boxW = 150 * s, gap = 5 * s;
    const f32 total = devices.size() * (boxW + gap) + 6 * s;
    const f32 maxScroll = std::max(0.f, total - area.w);
    stripScroll_ = clampv(stripScroll_, 0.f, maxScroll);
    bool wheelUsed = false;

    f32 x = area.x + 6 * s - stripScroll_;
    for (size_t i = 0; i < devices.size(); ++i) {
        DeviceModel& d = devices[i];
        Rect box{x, area.y + 4 * s, boxW, area.h - 9 * s};
        x += boxW + gap;
        if (box.right() < area.x || box.x > area.right()) continue;

        const bool sel = (int)i == selDevice_;
        // Claim hot for the whole box first so the controls drawn afterwards
        // can take it back — last setHot() of the frame wins.
        const u64 bid = uiId(11, (int)i, 2);
        const bool hotBox = ui_.setHot(bid, box) && ui_.isHot(bid);
        rend_.roundRect(box, 3 * s, sel ? pal::gridBg : pal::panelAlt);
        if (sel) rend_.roundRectOutline(box, 3 * s, 1 * s, pal::accent);

        Rect title{box.x, box.y, box.w, 16 * s};
        rend_.rect({title.x + 2 * s, title.y + 3 * s, 3 * s, title.h - 6 * s}, tc);

        // Both controls are glyph-drawn rather than lettered: at this size the
        // font ellipsises anything longer than a character or two.
        Rect xr{title.right() - 17 * s, title.y + 2 * s, 14 * s, 12 * s};
        Rect br{xr.x - 20 * s, title.y + 2 * s, 18 * s, 12 * s};

        Rect nameR{title.x + 9 * s, title.y, br.x - title.x - 11 * s, title.h};
        rend_.pushClip(nameR);
        rend_.textIn(fBold_, nameR, d.desc.name.c_str(), sel ? pal::text : pal::textDim,
                     Align::Left, 0);
        rend_.popClip();

        // Bypass lives on the instance, so the chain does not have to be
        // republished; setBypassed() is GUI-safe per the host contract.
        const bool wasBypass = d.bypass;
        if (ui_.squareToggle(uiId(11, (int)i, 0), br, "", &d.bypass, pal::meterAmber)) {
            undoPointWith("bypass", d.bypass, wasBypass);
            if (d.inst) d.inst->setBypassed(d.bypass);
        }
        rend_.circle(br.cx(), br.cy(), 3.5f * s,
                     d.bypass ? pal::textOnClip : pal::playGreen);   // lit = active
        const bool xHot = ui_.button(uiId(11, (int)i, 1), xr, "");
        {
            const f32 k = 3.f * s;
            const Col xc = pal::textDim;
            rend_.line(xr.cx() - k, xr.cy() - k, xr.cx() + k, xr.cy() + k, 1.2f * s, xc);
            rend_.line(xr.cx() - k, xr.cy() + k, xr.cx() + k, xr.cy() - k, 1.2f * s, xc);
        }
        if (xHot) {
            // The instance is retired with the outgoing chain, so undoing this
            // loads the plugin again and applies the parameters the snapshot
            // carries - see materializeDevices. What a plugin holds beyond its
            // parameters does not survive, which is the same trade a saved set
            // makes and is documented as such in app.h.
            undoPoint("remove device");
            removeDevice(devOwner_, (int)i);
            rend_.popClip();
            return;                       // the device list changed under us
        }
        if (hotBox && in.pressed[0]) { selDevice_ = (int)i; paramScroll_ = 0.f; }

        Rect body{box.x + 4 * s, title.bottom() + 2 * s, box.w - 8 * s,
                  box.bottom() - title.bottom() - 6 * s};
        if (!d.inst) {
            // A device restored from a set whose plugin is not installed here.
            // It holds its place and its saved values (see DeviceModel), so the
            // chain comes back intact on a machine that has the plugin.
            rend_.pushClip(body);
            rend_.textIn(fSmall_, {body.x, body.y + 2 * s, body.w, 12 * s},
                         "plugin not installed", pal::armRed, Align::Left, 0);
            rend_.textIn(fSmall_, {body.x, body.y + 15 * s, body.w, 12 * s},
                         d.desc.uri.c_str(), pal::textFaint, Align::Left, 0);
            rend_.popClip();
            continue;
        }

        if (!sel) {
            // Unselected devices stay compact; only one chain slot is edited at
            // a time, like Live collapsing the devices you are not touching.
            char buf[64];
            snprintf(buf, sizeof buf, "%d params", d.inst->paramCount());
            rend_.pushClip(body);
            if (!d.desc.vendor.empty())
                rend_.textIn(fSmall_, {body.x, body.y + 2 * s, body.w, 12 * s},
                             d.desc.vendor.c_str(), pal::textFaint, Align::Left, 0);
            rend_.textIn(fSmall_, {body.x, body.y + 15 * s, body.w, 12 * s}, buf,
                         pal::textFaint, Align::Left, 0);
            rend_.popClip();
            continue;
        }

        // --- parameters of the selected device ---
        const int n = d.inst->paramCount();
        const int cols = 3;
        // 43px is knob (32) + label (11): three rows land exactly inside the
        // panel, so a device with nine or fewer controls never has to scroll.
        const f32 cw = body.w / (f32)cols, chh = 43 * s;
        const int rows = (n + cols - 1) / cols;
        const f32 pMax = std::max(0.f, rows * chh - body.h);
        if (ui_.hovered(body) && in.wheel != 0.f) {
            paramScroll_ -= in.wheel * chh * 0.5f;
            wheelUsed = true;
        }
        paramScroll_ = clampv(paramScroll_, 0.f, pMax);

        rend_.pushClip(body);
        if (n == 0)
            rend_.textIn(fSmall_, body, "no parameters", pal::textFaint, Align::Center);
        for (int p = 0; p < n; ++p) {
            Rect cell{body.x + (p % cols) * cw, body.y - paramScroll_ + (p / cols) * chh, cw, chh};
            if (cell.bottom() < body.y || cell.y > body.bottom()) continue;
            const ParamInfo& info = d.inst->paramInfo(p);
            Rect lbl{cell.x, cell.bottom() - 11 * s, cell.w, 10 * s};

            // Both controls edit a copy and hand the result to the instance, so
            // the value the snapshot reads (serializeDevices asks the instance)
            // is still the old one when the entry is taken. A knob drag
            // coalesces on the widget's id, as everywhere else.
            // Both also report the move to autoCapture (docs/AUTOMATION.md
            // §5.1). Unconditionally, and only for a TRACK chain: a clip
            // envelope may only automate its own track's devices (§4.2 step 2,
            // decision #2), so a return's or the master's knob has no clip to
            // record into and no address a lane could name. Everything else —
            // the arm, the transport, which clip is playing, the thinning —
            // is autoCapture's decision, kept in one place on purpose. The
            // value is the plugin's own units (§2.3), and the knob's id is the
            // gesture, so one drag is one pass and one undo entry.
            const u64 wid = uiId(12, (int)i * 256 + p, 0);
            const bool ownTrack = ownIsTrack(devOwner_);
            if (info.isBool) {
                Rect tg{cell.cx() - 11 * s, cell.y + 8 * s, 22 * s, 14 * s};
                bool on = d.inst->getParam(p) > 0.5f;
                if (ui_.squareToggle(wid, tg, "", &on, pal::accent)) {
                    undoPoint(info.name.c_str());
                    const f32 nv = on ? info.max : info.min;
                    d.inst->setParam(p, nv);
                    if (ownTrack)
                        autoCapture(addr::deviceParam(ses_.tracks[devOwner_].uid, d.uid, info.id),
                                    nv, wid);
                }
            } else {
                Rect kr{cell.cx() - 16 * s, cell.y + 2 * s, 32 * s, 32 * s};
                f32 v = d.inst->getParam(p);
                if (ui_.knob(wid, kr, &v, info.min, info.max,
                             info.def, info.isInt ? "%.0f" : "%.2f")) {
                    undoPoint(info.name.c_str());
                    d.inst->setParam(p, v);
                    if (ownTrack)
                        autoCapture(addr::deviceParam(ses_.tracks[devOwner_].uid, d.uid, info.id),
                                    v, wid);
                }
            }

            rend_.pushClip(lbl);
            rend_.textIn(fSmall_, lbl, info.name.c_str(), pal::textDim, Align::Center, 0);
            rend_.popClip();
        }
        rend_.popClip();
    }

    // The strip scrolls horizontally on a plain wheel, unless the pointer was
    // over a parameter grid that wanted the notch for itself.
    if (!wheelUsed && maxScroll > 0.f && ui_.hovered(area) && in.wheel != 0.f)
        stripScroll_ = clampv(stripScroll_ - in.wheel * 60.f * s, 0.f, maxScroll);

    rend_.popClip();
}


} // namespace lat
