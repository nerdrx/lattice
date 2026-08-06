// Time signatures, GUI side: the ruler, the grid, the readout and the editor.
//
// THE PROPERTY THIS FILE EXISTS FOR is one sentence: a bar line that is DRAWN
// and a bar line that is PLAYED cannot disagree. The engine's metronome, its
// launch quantum and its position readout all go through sigBeatOfBar /
// sigPosAt / sigNextBarLine in engine.cpp; so, now, does every pixel the
// arrangement's ruler and grid put on screen, through SigMap in
// src/ui/timeaxis.h. The assertions below check that the forwarding is real --
// that every line the view emits is a bar line the engine agrees is one, that
// none is missing, and that the ruler's bar numbers are the readout's.
//
// The second property, and the one a user would notice first if it broke: an
// arrangement item and the loop brace live in BEATS. Re-barring a piece must
// move neither. §"the interaction that matters most".
//
// BUILD (there is no Makefile target -- this wave does not own the Makefile):
//   tools/build_timesig_view_test.sh
//
// and the Makefile line it is owed is spelled out in that script.
#include "../src/ui/arrange.h"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace lat;

static int g_pass = 0, g_fail = 0;

static void check(bool ok, const char* what) {
    if (ok) { ++g_pass; return; }
    ++g_fail;
    std::printf("  FAIL  %s\n", what);
}
static void checkf(bool ok, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void checkf(bool ok, const char* fmt, ...) {
    if (ok) { ++g_pass; return; }
    ++g_fail;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::printf("  FAIL  %s\n", buf);
}

// ---------------------------------------------------------------------------
// scaffolding
// ---------------------------------------------------------------------------

struct Line { f64 beat; int level; };

static SigMap mapOf(const std::vector<SigChange>& v, int fbNum = 4, int fbDen = 4) {
    SigMap m;
    m.v = v.empty() ? nullptr : v.data();
    m.n = (int)v.size();
    m.fbNum = fbNum;
    m.fbDen = fbDen;
    return m;
}

// A normalized map from a list of (bar, num, den). Built through Session so the
// clamps and the dedupe under test are the ones the editor calls.
struct SC { int bar, num, den; };
static std::vector<SigChange> build(std::initializer_list<SC> in) {
    Session s;
    for (const SC& c : in) s.setSignature(c.bar, c.num, c.den);
    if (s.sigs.empty()) s.normalizeSigs();
    return s.sigs;
}

static std::vector<Line> generalLines(const SigMap& m, const TimeAxis& ta,
                                      const Rect& r, f32 minPx) {
    std::vector<Line> out;
    forEachGridLine(m, ta.pxPerBeat, minPx, (f64)xToBeat(ta, r.x),
                    (f64)xToBeat(ta, r.right()),
                    [&](f64 b, int lv) { out.push_back({b, lv}); });
    return out;
}

static std::vector<Line> uniformLines(const TimeAxis& ta, const Rect& r,
                                      int bpb, f32 minPx) {
    std::vector<Line> out;
    forEachUniformGridLine(ta, r, bpb, minPx,
                           [&](f64 b, f32, int lv) { out.push_back({b, lv}); });
    return out;
}

// Both walks reach one step past their rect in different directions, so a fair
// comparison is over the beats both of them are asked about.
static std::vector<Line> window(const std::vector<Line>& v, f64 lo, f64 hi) {
    std::vector<Line> out;
    for (const Line& l : v)
        if (l.beat >= lo - 1e-9 && l.beat <= hi + 1e-9) out.push_back(l);
    return out;
}

// ---------------------------------------------------------------------------
// 1. the general walk IS the uniform walk, wherever the uniform walk applies
//
// timeaxis.h delegates a one-entry x/4 map to the loop that has always drawn it,
// so an existing set renders bit-identically. That delegation is only honest if
// the general walk would have drawn the same thing -- otherwise it is two
// definitions of a grid with a switch between them, which is exactly the shape
// that lets a ruler and a metronome drift.
// ---------------------------------------------------------------------------

static void testUniformAgreement() {
    const Rect r{140.f, 40.f, 1200.f, 600.f};
    const f32 minPx[] = {0.5f, 7.f};
    const int  bpbs[] = {1, 2, 3, 4, 5, 6, 7, 12};
    const f32  zooms[] = {8.f, 11.f, 16.f, 24.f, 32.f, 64.f, 100.f, 128.f, 256.f, 512.f};
    const f32  scrolls[] = {0.f, 37.f, 512.f, 5000.f};

    for (f32 mp : minPx)
    for (int bpb : bpbs)
    for (f32 z : zooms)
    for (f32 sc : scrolls) {
        const TimeAxis ta{r.x, z, sc};
        const std::vector<SigChange> v = build({{0, bpb, 4}});
        const f64 lo = (f64)xToBeat(ta, r.x), hi = (f64)xToBeat(ta, r.right());
        const std::vector<Line> a = window(uniformLines(ta, r, bpb, mp), lo, hi);
        const std::vector<Line> b = window(generalLines(mapOf(v), ta, r, mp), lo, hi);
        bool ok = a.size() == b.size();
        for (size_t i = 0; ok && i < a.size(); ++i)
            ok = std::fabs(a[i].beat - b[i].beat) < 1e-9 && a[i].level == b[i].level;
        checkf(ok, "uniform %d/4 agrees with the map walk (zoom %.0f scroll %.0f min %.1f): "
                   "%zu vs %zu lines", bpb, (double)z, (double)sc, (double)mp,
               a.size(), b.size());
    }
}

// ---------------------------------------------------------------------------
// 2. drawn == played
// ---------------------------------------------------------------------------

static void testDrawnEqualsPlayed() {
    struct Case { const char* name; std::vector<SigChange> m; };
    const std::vector<Case> cases = {
        {"4/4",              build({{0, 4, 4}})},
        {"3/4",              build({{0, 3, 4}})},
        {"7/8",              build({{0, 7, 8}})},
        {"5/4 then 7/8",     build({{0, 5, 4}, {8, 7, 8}})},
        {"4/4 7/8 3/4 12/8", build({{0, 4, 4}, {8, 7, 8}, {12, 3, 4}, {20, 12, 8}})},
        {"1/4 then 3/2",     build({{0, 1, 4}, {6, 3, 2}})},
    };

    const Rect r{140.f, 40.f, 1200.f, 600.f};
    for (const Case& c : cases) {
        const SigMap m = mapOf(c.m);
        const RtSig* sv = c.m.data();
        const int    sn = (int)c.m.size();

        for (f32 z : {8.f, 16.f, 32.f, 64.f, 128.f})
        for (f32 sc : {0.f, 400.f, 3000.f}) {
            const TimeAxis ta{r.x, z, sc};
            const f64 lo = (f64)xToBeat(ta, r.x), hi = (f64)xToBeat(ta, r.right());
            const std::vector<Line> lines = generalLines(m, ta, r, 7.f);

            // (a) Every BAR line the view draws is a downbeat the engine agrees
            //     with: sigPosAt lands on unit 0, sixteenth 0, of a bar whose
            //     own start is that very beat.
            bool ok = true;
            for (const Line& l : lines) {
                if (l.level != 2) continue;
                const BarPos p = sigPosAt(sv, sn, l.beat + 1e-9);
                if (p.beat != 0 || p.sixteenth != 0) { ok = false; break; }
                if (std::fabs(sigBeatOfBar(sv, sn, (f64)p.bar) - l.beat) > 1e-9) { ok = false; break; }
                if (std::fabs(p.barStart - l.beat) > 1e-9) { ok = false; break; }
            }
            checkf(ok, "%s: every drawn bar line is an engine downbeat (zoom %.0f scroll %.0f)",
                   c.name, (double)z, (double)sc);

            // (b) Every UNIT line is on a unit boundary and not on a bar line.
            ok = true;
            for (const Line& l : lines) {
                if (l.level != 1) continue;
                const BarPos p = sigPosAt(sv, sn, l.beat + 1e-9);
                if (p.beat == 0 || p.sixteenth != 0) { ok = false; break; }
            }
            checkf(ok, "%s: every drawn beat line is an engine unit (zoom %.0f scroll %.0f)",
                   c.name, (double)z, (double)sc);

            // (c) NOTHING IS MISSING. Every bar whose start is in view and whose
            //     number is a multiple of the stride was drawn. A ruler that put
            //     its lines in the right places but skipped one would pass (a).
            const i64 stride = barStrideFor(m, ta.pxPerBeat, 7.f);
            i64 drawn = 0;
            for (const Line& l : lines) if (l.level == 2) ++drawn;
            i64 want = 0;
            const i64 barLo = (i64)std::floor(sigBarOfBeat(sv, sn, std::max(0.0, lo)));
            const i64 barHi = (i64)std::floor(sigBarOfBeat(sv, sn, hi));
            for (i64 bar = (barLo / stride) * stride; bar <= barHi; bar += stride) {
                const f64 bs = sigBeatOfBar(sv, sn, (f64)bar);
                if (bs >= std::max(0.0, lo) - 1e-9 && bs <= hi + 1e-9) ++want;
            }
            // The walk starts at the bar CONTAINING the left edge, which can be
            // one bar earlier than the first one inside it.
            checkf(drawn == want || drawn == want + 1,
                   "%s: %lld bar lines drawn, %lld expected (zoom %.0f scroll %.0f stride %lld)",
                   c.name, (long long)drawn, (long long)want, (double)z, (double)sc,
                   (long long)stride);

            // (d) THE LAUNCH QUANTUM. sigNextBarLine is what "1 Bar" means to the
            //     engine; from anywhere inside a bar it must land on the very
            //     next line the ruler drew.
            if (stride == 1) {
                ok = true;
                for (const Line& l : lines) {
                    if (l.level != 2) continue;
                    const f64 inside = l.beat + 1e-6;
                    const f64 next = sigNextBarLine(sv, sn, inside, 1);
                    const f64 wantNext = sigBeatOfBar(
                        sv, sn, (f64)(sigPosAt(sv, sn, inside).bar + 1));
                    if (std::fabs(next - wantNext) > 1e-6) { ok = false; break; }
                }
                checkf(ok, "%s: the 1-bar quantum lands on the next drawn bar line", c.name);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 3. SigMap is Session, and Session is the engine
// ---------------------------------------------------------------------------

static void testForwarding() {
    Session s;
    s.setSignature(0, 4, 4);
    s.setSignature(8, 7, 8);
    s.setSignature(12, 3, 4);
    const SigMap m = sigMapOf(s);

    bool ok = true;
    for (f64 b = 0.0; b < 120.0 && ok; b += 0.125) {
        const BarPos a = m.posAt(b), c = s.barPosAt(b);
        ok = a.bar == c.bar && a.beat == c.beat && a.sixteenth == c.sixteenth &&
             a.num == c.num && a.den == c.den &&
             std::fabs(m.barOfBeat(b) - s.barOfBeat(b)) < 1e-12;
    }
    check(ok, "SigMap answers exactly what Session's own forwarders do");

    ok = true;
    for (i64 bar = 0; bar < 64 && ok; ++bar)
        ok = std::fabs(m.beatOfBar((f64)bar) - s.beatOfBar((f64)bar)) < 1e-12 &&
             m.sigAtBar(bar).num == s.sigAtBar((int)bar).num &&
             m.sigAtBar(bar).den == s.sigAtBar((int)bar).den;
    check(ok, "SigMap::beatOfBar / sigAtBar agree with Session's");

    // An EMPTY map is not 4/4: it is one entry of sigNum/sigDen at bar 0.
    Session e;
    e.sigNum = 7; e.sigDen = 8;          // exactly what a test or a fresh set writes
    const SigMap em = sigMapOf(e);
    check(em.empty() && em.count() == 1 && em.entry(0).num == 7 && em.entry(0).den == 8,
          "an empty sigs vector reads as one entry of sigNum/sigDen");
    check(std::fabs(em.beatOfBar(2.0) - 7.0) < 1e-12,
          "and measures in it: two bars of 7/8 are 7 beats");
}

// ---------------------------------------------------------------------------
// 4. the readout, and that a 4/4 set reads exactly as it always did
// ---------------------------------------------------------------------------

static void testReadout() {
    Session s;                                    // plain 4/4, untouched
    const SigMap m = sigMapOf(s);
    bool ok = true;
    std::string bad;
    for (f64 b = 0.0; b < 400.0; b += 0.0625) {
        // The expression this wave replaced, verbatim.
        char was[48], now[48];
        std::snprintf(was, sizeof was, "%d.%d.%d",
                      (int)std::floor(b / s.sigNum) + 1,
                      (int)std::floor(std::fmod(b, (f64)s.sigNum)) + 1,
                      (int)std::floor(std::fmod(b, 1.0) * 4.0) + 1);
        const BarPos p = m.posAt(b);
        std::snprintf(now, sizeof now, "%d.%d.%d", p.bar + 1, p.beat + 1, p.sixteenth + 1);
        if (std::strcmp(was, now) != 0) {
            ok = false;
            bad = std::string(was) + " -> " + now;
            break;
        }
    }
    checkf(ok, "in 4/4 the readout is character-for-character what it was (%s)",
           bad.empty() ? "" : bad.c_str());

    // And in 7/8 it counts EIGHTHS, which is the whole point: bar 2 beat 1 of
    // 7/8 is beat 3.5, not beat 7.
    Session t;
    t.setSignature(0, 7, 8);
    const BarPos p = sigMapOf(t).posAt(3.5);
    check(p.bar == 1 && p.beat == 0 && p.sixteenth == 0 && p.num == 7 && p.den == 8,
          "7/8: beat 3.5 reads 2.1.1");
    const BarPos q = sigMapOf(t).posAt(4.0);
    check(q.bar == 1 && q.beat == 1, "7/8: beat 4.0 reads 2.2.x");
}

// ---------------------------------------------------------------------------
// 5. THE INTERACTION THAT MATTERS MOST
//
// An arrangement item lives in beats. A signature change must not move it in
// time -- only the bar it is displayed at may change. Same for the loop brace.
// ---------------------------------------------------------------------------

static void testNothingMovesInTime() {
    Session s;
    TrackModel tr;
    const f64 starts[] = {0.0, 4.0, 8.0, 13.25, 32.0};
    for (f64 st : starts) {
        ArrangeClip c;
        c.uid = s.newUid();
        c.start = st;
        c.length = 3.5;
        tr.arrange.push_back(c);
    }
    s.tracks.push_back(std::move(tr));
    s.loopStart = 8.0;
    s.loopEnd   = 16.0;
    s.loopOn    = true;

    std::vector<f64> beforeStart, beforeEnd;
    for (const ArrangeClip& c : s.tracks[0].arrange) {
        beforeStart.push_back(c.start);
        beforeEnd.push_back(c.end());
    }
    const f64 loopA = s.loopStart, loopB = s.loopEnd;
    // Where each item DISPLAYS, before.
    std::vector<f64> barBefore;
    for (const ArrangeClip& c : s.tracks[0].arrange) barBefore.push_back(s.barOfBeat(c.start));

    // Re-bar: 7/8 from bar 2 on. Every item is now in a different bar and not
    // one of them has moved.
    s.setSignature(2, 7, 8);

    bool moved = false;
    for (size_t i = 0; i < s.tracks[0].arrange.size(); ++i) {
        const ArrangeClip& c = s.tracks[0].arrange[i];
        if (std::fabs(c.start - beforeStart[i]) > 0.0) moved = true;
        if (std::fabs(c.end() - beforeEnd[i]) > 0.0) moved = true;
    }
    check(!moved, "a signature change moves no arrangement item in time");
    check(s.loopStart == loopA && s.loopEnd == loopB && s.loopOn,
          "a signature change moves no loop brace");

    bool renumbered = false;
    for (size_t i = 0; i < s.tracks[0].arrange.size(); ++i)
        if (std::fabs(s.barOfBeat(s.tracks[0].arrange[i].start) - barBefore[i]) > 1e-9)
            renumbered = true;
    check(renumbered, "...but the bars they are DISPLAYED at did change");

    // And the brace still spans the beats it always did, measured through the
    // new map: beat 8 is bar 3 of 4/4 and something else in 7/8, but it is
    // still beat 8.
    check(std::fabs(s.beatOfBar(s.barOfBeat(8.0)) - 8.0) < 1e-9,
          "beat 8 round-trips through the new map");
}

// ---------------------------------------------------------------------------
// 6. the editor's verbs, which are session.h's
// ---------------------------------------------------------------------------

static void testEditor() {
    Session s;
    check(!s.removeSignature(0), "bar 0 cannot be removed");
    s.setSignature(0, 3, 4);
    check(s.sigNum == 3 && s.sigDen == 4 && s.sigs.size() == 1,
          "setSignature(0, ...) is the session signature");

    s.setSignature(8, 7, 8);
    s.setSignature(8, 5, 8);                      // last wins, no second entry
    check(s.sigs.size() == 2 && s.sigs[1].num == 5,
          "a second change at one bar replaces the first");

    // The clamps are session.h's and they round a denominator DOWN.
    s.setSignature(12, 999, 3);
    check(s.sigAtBar(12).num == kSigNumMax && s.sigAtBar(12).den == 2,
          "clamps: numerator to kSigNumMax, denominator down to a power of two");

    check(s.removeSignature(12) && s.sigs.size() == 2, "removeSignature drops it");
    check(!s.removeSignature(9), "removing a bar with no change there does nothing");

    // The map the engine would be handed passes the engine's own gate.
    const std::vector<SigChange> pub = normalizedSigMap(s.sigs, s.sigNum, s.sigDen);
    check(sigMapValid(pub.data(), (int)pub.size()),
          "the map the editor produces is one the engine accepts");

    // Two changes make the map non-uniform, which is what turns the ruler's
    // markers on -- and one entry never does.
    check(!mapOf(pub).uniform(), "two entries is not a uniform map");
    Session u;
    check(sigMapOf(u).uniform(), "a set that has never been re-barred is uniform");
}

// ---------------------------------------------------------------------------

int main() {
    std::printf("nxtakt time-signature view tests\n");
    testUniformAgreement();
    testDrawnEqualsPlayed();
    testForwarding();
    testReadout();
    testNothingMovesInTime();
    testEditor();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
