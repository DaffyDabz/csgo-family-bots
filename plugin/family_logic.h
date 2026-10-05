// family_logic.h: the PURE logic of the family bots (no engine calls, no memory writes), shared by family_brain.cpp and
// the unit tests (plugin/test_pick.cpp). STEP 12 (Job 5, 2026-09-26): each part of the job keeps its decision rules here
// so they can be tested without the game; family_brain.cpp does the sensing and the engine levers.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace fl
{
struct V3
{
    float x, y, z;
};
inline float Clamp01(float x) { return x < 0.f ? 0.f : x > 1.f ? 1.f : x; }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float Dist(const V3 &a, const V3 &b) { return sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }
inline float Dist2(const V3 &a, const V3 &b) { return sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }
inline float Wrap180(float a)
{
    // fmod, never a subtract loop (a garbage 1e30 - 360 == 1e30 hung a test server in step 11)
    if (!std::isfinite(a) || fabsf(a) > 1e6f) return 1e6f;
    a = fmodf(a + 180.f, 360.f);
    if (a < 0) a += 360.f;
    return a - 180.f;
}
inline float YawTo(const V3 &a, const V3 &b) { return atan2f(b.y - a.y, b.x - a.x) * 57.29578f; }
inline float PitchTo(const V3 &a, const V3 &b)   // Source pitch: positive = looking down
{
    return atan2f(-(b.z - a.z), std::max(1.f, Dist2(a, b))) * 57.29578f;
}
// one frame of a turn: from `cur` towards `want` by at most `step` degrees (yaw wraps)
inline float TurnStep(float cur, float want, float step)
{
    float d = Wrap180(want - cur);
    if (d > step) d = step;
    if (d < -step) d = -step;
    return Wrap180(cur + d);
}

// ==== STEP 12B hearing ==============================================================================================
// What a bot can hear, and how it reacts, by rank. The game only plays a footstep sound (and fires player_footstep) for a
// RUNNING player: shift-walking and crouching are silent (step 10a measured ~1 step a round for a walking bot). The
// brain still re-checks the stepping player (walk flag, duck flag, speed) so walking and crouching stay silent even if
// an event slips through.
enum NoiseKind { NZ_STEP, NZ_JUMP, NZ_LAND, NZ_SHOT, NZ_SHOT_SIL, NZ_RELOAD, NZ_PLANT, NZ_DEFUSE, NZ_ZOOM, NZ_N };
inline const char *NoiseName(int k)
{
    static const char *n[NZ_N] = {"step", "jump", "land", "shot", "silenced", "reload", "plant", "defuse", "zoom"};
    return k >= 0 && k < NZ_N ? n[k] : "?";
}
// audible ranges (units, straight distance). Running footsteps 1100 = the range the stock CS bot uses for a footstep
// (CS:S/CS:GO bot code lineage, OnPlayerFootstep); jump/landing like a step; an unsilenced shot is heard across most of
// the map - 1800 is how far a bot TURNS to one (a farther shot is only a team call); a silenced shot / reload / zoom
// are short; bomb plant and defuse sounds like a step but always matter. Each is a test hook (hear_r_<kind>).
struct HearRanges
{
    float r[NZ_N];
};
inline HearRanges DefaultRanges()
{
    HearRanges h;
    const float d[NZ_N] = {1100, 900, 1000, 1800, 600, 700, 1100, 1100, 450};
    for (int i = 0; i < NZ_N; i++) h.r[i] = d[i];
    return h;
}
const float kSilentSpeed = 140.f;   // a step under this speed is a walk (shift = 0.52 x 250 = 130): silent
inline int NoisePrio(int k) { return k == NZ_PLANT || k == NZ_DEFUSE ? 3 : (k == NZ_SHOT || k == NZ_SHOT_SIL) ? 2 : 1; }
inline bool Audible(const HearRanges &hr, int k, float d, float speed, bool walking, bool ducking)
{
    if (k < 0 || k >= NZ_N || !(d >= 0.f)) return false;
    if (k == NZ_STEP && (walking || ducking || speed < kSilentSpeed)) return false;   // walking and crouching are silent
    return d <= hr.r[k];
}
// the reaction of a bot with hearing dial h (0 = Silver I .. 1 = Global Elite): every rank reacts; higher ranks react
// sooner, turn faster, pre-aim the corner the enemy will come round (not just the direction) with a smaller error, hold
// the angle longer and stop to listen more often; rnd01 = a random draw for the jitter.
struct HearReact
{
    float delay, errDeg, turnSpeed, holdFor, stopChance, callChance;
    bool corner;
};
inline HearReact HearFor(float h, float rnd01)
{
    h = Clamp01(h);
    HearReact r;
    r.delay = Lerp(0.55f, 0.12f, h) + Clamp01(rnd01) * Lerp(0.20f, 0.05f, h);
    r.errDeg = Lerp(22.f, 3.f, h);
    r.turnSpeed = Lerp(260.f, 900.f, h);
    r.holdFor = Lerp(0.9f, 2.6f, h);
    r.stopChance = Lerp(0.f, 0.8f, h);
    r.callChance = Lerp(0.3f, 1.f, h);
    r.corner = h >= 0.25f;   // ~elo 950 up (a Gold Nova I bot sits at ~0.33)
    return r;
}
// a new noise replaces the one a bot is reacting to when it matters more: a higher priority, or the same priority and
// clearly closer, or the old reaction is stale (older than 0.8 s)
inline bool HearBetter(int newPrio, float newDist, int curPrio, float curDist, float curAge)
{
    if (curPrio <= 0) return true;
    if (newPrio != curPrio) return newPrio > curPrio;
    return newDist < curDist * 0.7f || curAge > 0.8f;
}
// the corner to pre-aim for a noise the bot cannot see: among nav hiding spots the bot CAN see (vis[i]) near the noise,
// the one closest to the noise that lies roughly in its direction (within 70 deg); -1 = none (aim at the noise itself)
inline int PickCorner(const V3 &bot, const V3 &noise, const std::vector<V3> &spots, const std::vector<char> &vis)
{
    int best = -1;
    float bd = 1e9f, want = YawTo(bot, noise), dn = Dist(bot, noise);
    for (size_t i = 0; i < spots.size() && i < vis.size(); i++)
    {
        if (!vis[i]) continue;
        const V3 &s = spots[i];
        float ds = Dist(s, noise);
        if (ds > 600.f || Dist(bot, s) < 120.f || Dist(bot, s) > dn + 150.f) continue;
        if (fabsf(Wrap180(YawTo(bot, s) - want)) > 70.f) continue;
        float score = ds + 0.25f * Dist(bot, s);
        if (score < bd) { bd = score; best = (int)i; }
    }
    return best;
}
// ==== STEP 12G reacting to fire =====================================================================================
// His words: "They don't move if they're getting shot at." A bot that is hit or shot at (an enemy's aim on it with a
// clear line) reacts, by its fire dial (footwork + react, profile-style: every rank reacts):
//   TURN  the shooter is outside its view: turn to him (fast), then the stock fight takes over
//   DODGE it fights him: move while it fights (the stock fight side-steps above Skill 0.5 - step 11 keeps every bot
//         under 0.5 so they spray; under fire the dial lifts it into the side-step band). Low ranks only up close
//         (inside 400 u the stock bot sprays at any Skill, so a dodging beginner still sprays - "ugly spray")
//   COVER it is losing the trade (hit to <= 55 hp, two hits, or one heavy hit): break line of sight to the nearest
//         hidden spot, wait, then re-peek (not inside 300 u: a point-blank fight is fought - step 11's panic)
// Planting / defusing bots stay on the job.
enum FireAct { FA_NONE, FA_TURN, FA_DODGE, FA_COVER };
inline const char *FireActName(int a)
{
    static const char *n[4] = {"none", "turn", "dodge", "cover"};
    return a >= 0 && a < 4 ? n[a] : "?";
}
struct FireSit
{
    float dial;       // 0 Silver I .. 1 Global Elite
    bool seen;        // the shooter is in its view with a clear line
    bool hit;         // it was hit (not just shot at)
    float health, dmg;
    int hits;         // hits taken in the last 1.5 s
    float dist;       // to the shooter
    bool cover;       // a hidden spot is within reach
    bool objective;   // planting / defusing
};
struct FireChoice
{
    int act;
    float delay, hold, skill, turnSpeed;
};
inline bool FireLosing(const FireSit &s) { return s.hit && (s.health <= 55.f || s.hits >= 2 || s.dmg >= 45.f); }
inline FireChoice FireDecide(const FireSit &s, float r1, float r2)
{
    float g = Clamp01(s.dial);
    FireChoice c;
    c.act = FA_NONE;
    c.delay = Lerp(0.40f, 0.10f, g);
    c.turnSpeed = Lerp(500.f, 1200.f, g);
    c.hold = Lerp(2.0f, 0.8f, g);      // behind cover this long, then the re-peek
    c.skill = Lerp(0.55f, 0.75f, g);   // the side-step band (the stock line is 0.5)
    if (s.objective) return c;
    bool cover = FireLosing(s) && s.cover && s.dist >= 300.f && r1 < Lerp(0.15f, 0.70f, g);
    if (!s.seen)
    {
        c.act = cover ? FA_COVER : FA_TURN;
        return c;
    }
    if (cover)
        c.act = FA_COVER;
    else if ((g >= 0.2f || s.dist < 400.f) && r2 < Lerp(0.20f, 0.80f, g))
        c.act = FA_DODGE;
    return c;
}
// the spot to break line of sight: hidden from the shooter (hidden[i]), 80..450 u away, not towards him (> 45 deg off
// the line to him) and not closer to him; nearest first, nav IN_COVER spots preferred; -1 = none
inline int PickCover(const V3 &bot, const V3 &shooter, const std::vector<V3> &spots, const std::vector<char> &hidden, const std::vector<char> &inCover)
{
    int best = -1;
    float bd = 1e9f, toS = YawTo(bot, shooter), ds = Dist2(bot, shooter);
    for (size_t i = 0; i < spots.size() && i < hidden.size(); i++)
    {
        if (!hidden[i]) continue;
        float d = Dist2(bot, spots[i]);
        if (d < 80.f || d > 450.f || fabsf(spots[i].z - bot.z) > 80.f) continue;
        if (fabsf(Wrap180(YawTo(bot, spots[i]) - toS)) < 45.f || Dist2(spots[i], shooter) < ds - 50.f) continue;
        float score = d - (i < inCover.size() && inCover[i] ? 60.f : 0.f);
        if (score < bd) { bd = score; best = (int)i; }
    }
    return best;
}
// ==== STEP 12C defenders ============================================================================================
// His words: "they do a lot of pushing as defenders... they should hold corners, wait, catch you slipping"; default two on
// each site + one lurker/rotator, "stacking is fine" sometimes, not the default; "they should really try to keep at least
// one planted on site" = one CT anchor always on each bombsite.
// CtRoles: the site of each CT slot (-1 = the mid / rotator). Slots 0..nSites-1 are the ANCHORS, one per site, then the
// second on each site, then the rotator (mid), then extras to the stacked (or first) site. A stack moves the second
// man of the other sites (never an anchor) to the stacked site, and the rotator too when there is no mid.
inline std::vector<int> CtRoles(int n, int nSites, bool hasMid, int stack)
{
    std::vector<int> r;
    if (n <= 0 || nSites <= 0) return r;
    if (stack >= nSites) stack = -1;
    for (int i = 0; i < nSites && (int)r.size() < n; i++) r.push_back(i);   // anchors
    if ((int)r.size() >= n) return r;
    std::vector<int> seconds;
    for (int i = 0; i < nSites; i++) seconds.push_back(i);
    bool rotator = hasMid && n >= nSites + 1 && (n - nSites) % 2 == 1 && nSites >= 2;   // 5 on 2 sites: 2 / 2 / 1
    int left = n - (int)r.size() - (rotator ? 1 : 0);
    if (stack >= 0)
    {
        // stacked: every second man goes to the stack site (other sites keep only their anchor)
        for (int i = 0; i < left; i++) r.push_back(stack);
    }
    else
        for (int i = 0; i < left; i++) r.push_back(seconds[i % nSites]);
    if (rotator) r.push_back(-1);
    return r;
}
// a smart CT team stacks sometimes: 12% base, +20% towards a site the Ts hit in 2 of their last 3 rounds (reads)
// recent = the sites the Ts hit, newest last (-1 = no hit); returns the stacked site or -1
inline int CtStackPick(const std::vector<int> &recent, int nSites, bool smart, float r1, float r2)
{
    if (!smart || nSites < 2) return -1;
    std::vector<int> cnt(nSites, 0);
    int from = std::max(0, (int)recent.size() - 3);
    for (int i = from; i < (int)recent.size(); i++)
        if (recent[i] >= 0 && recent[i] < nSites) cnt[recent[i]]++;
    int best = 0;
    for (int i = 1; i < nSites; i++)
        if (cnt[i] > cnt[best]) best = i;
    float p = 0.12f + (cnt[best] >= 2 ? 0.20f : 0.f);
    if (r1 >= p) return -1;
    return cnt[best] >= 2 ? best : (int)(r2 * nSites) % nSites;
}
// "T ground": the Ts reach this spot at least 3 s before the CTs can (nav earliest-occupy times, 1/10 s) - a CT there
// before the Ts have shown on a site is pushing
inline bool TGround(int occT, int occCT) { return occT + 30 < occCT; }

// ==== STEP 12D/H/J round plans ======================================================================================
// D: at freeze time one T bot calls the plan in team chat ("A push", "A split", "B push", "B split", "B to A", "A to B",
// "mid to B"); J: plans vary by map and round, and a mid-heavy plan (60%+ of the Ts through a *mid* approach) is rare:
// never two in three rounds ("not straight down mid doors two out of three rounds"); H: the human bomb carrier's
// direction overrules the call.
enum PlanType { PL_PUSH, PL_SPLIT, PL_FAKE };
struct PlanSite
{
    std::string label;
    std::vector<char> apMid;   // per approach: 1 = through a *mid* place
};
struct TPlan
{
    int type = PL_PUSH, site = 0, from = -1, ap1 = 0, ap2 = -1;
    float midShare = 0;        // share of the Ts that go through mid
};
inline bool SamePlan(const TPlan &a, const TPlan &b) { return a.type == b.type && a.site == b.site && a.from == b.from && a.ap1 == b.ap1 && a.ap2 == b.ap2; }
inline bool MidHeavy(const TPlan &p) { return p.midShare >= 0.6f; }
inline int MainAp(const PlanSite &s)   // the site's first approach that avoids mid (else the first)
{
    for (size_t i = 0; i < s.apMid.size(); i++)
        if (!s.apMid[i]) return (int)i;
    return 0;
}
inline int SplitMain(int n) { return std::max(1, (int)ceilf(0.6f * n)); }   // 5 Ts: 3 + 2
inline std::string PlanText(const TPlan &p, const std::vector<PlanSite> &s)
{
    if (p.site < 0 || p.site >= (int)s.size()) return "?";
    const std::string &L = s[p.site].label;
    if (p.type == PL_SPLIT) return L + " split";
    if (p.type == PL_FAKE && p.from >= 0 && p.from < (int)s.size()) return s[p.from].label + " to " + L;
    bool viaMid = p.ap1 >= 0 && p.ap1 < (int)s[p.site].apMid.size() && s[p.site].apMid[p.ap1] && p.ap1 != MainAp(s[p.site]);
    return viaMid ? "mid to " + L : L + " push";
}
// every plan the team could call this round, with its weight (before the history)
struct PlanCand
{
    TPlan p;
    float w;
};
inline std::vector<PlanCand> PlanCands(const std::vector<PlanSite> &s, int nT, float smart)
{
    std::vector<PlanCand> c;
    int ns = (int)s.size();
    float split = 0.4f + 0.6f * Clamp01(smart), fake = Clamp01(smart) >= 0.25f ? 0.5f : 0.15f;
    for (int i = 0; i < ns; i++)
    {
        int na = std::max(1, (int)s[i].apMid.size()), m = MainAp(s[i]);
        auto mid = [&](int k) { return k >= 0 && k < (int)s[i].apMid.size() && s[i].apMid[k] ? 1.f : 0.f; };
        TPlan p;
        p.site = i;
        p.ap1 = m;
        p.midShare = mid(m);
        c.push_back({p, 1.f});
        for (int k = 0; k < na; k++)   // "mid to X": a real play, rarer
            if (k != m && mid(k) > 0)
            {
                TPlan q = p;
                q.ap1 = k;
                q.midShare = 1.f;
                c.push_back({q, 0.35f});
            }
        if (na >= 2 && nT >= 3)
        {
            int g1 = SplitMain(nT), g2 = nT - g1, pairs = na - 1;
            for (int k = 0; k < na; k++)
                if (k != m)
                {
                    TPlan q = p;
                    q.type = PL_SPLIT;
                    q.ap2 = k;
                    q.midShare = (mid(m) * g1 + mid(k) * g2) / (float)nT;
                    c.push_back({q, split / pairs});
                }
        }
        if (ns >= 2 && nT >= 3)
            for (int f = 0; f < ns; f++)
                if (f != i)
                {
                    TPlan q = p;
                    q.type = PL_FAKE;
                    q.from = f;
                    q.ap1 = MainAp(s[f]);
                    q.midShare = s[f].apMid.empty() ? 0.f : (float)s[f].apMid[q.ap1];
                    c.push_back({q, fake / (ns - 1)});
                }
    }
    return c;
}
// the history (this map, newest last) shapes the weights: the same call as last round x0.25 (same kind at the same site
// x0.4) and never three rounds running, as two rounds ago x0.6, the same site three rounds running x0.4, a site not hit
// in the last 3 x1.15; midCap: no mid-heavy plan when one of the last 2 was (so never 2 in any 3 rounds), and
// "everyone through mid" x0.5 always.
inline float PlanWeight(const PlanCand &c, const std::vector<TPlan> &hist, bool midCap)
{
    float w = c.w;
    size_t n = hist.size();
    if (n >= 1)
    {
        if (SamePlan(c.p, hist[n - 1])) w *= 0.25f;
        else if (c.p.type == hist[n - 1].type && c.p.site == hist[n - 1].site) w *= 0.4f;
        if (n >= 2 && SamePlan(c.p, hist[n - 1]) && SamePlan(c.p, hist[n - 2])) w = 0.f;
    }
    if (n >= 2 && c.p.type == hist[n - 2].type && c.p.site == hist[n - 2].site) w *= 0.6f;
    if (n >= 2 && hist[n - 1].site == c.p.site && hist[n - 2].site == c.p.site) w *= 0.4f;
    bool recent = false;
    for (size_t i = n > 3 ? n - 3 : 0; i < n; i++) recent = recent || hist[i].site == c.p.site;
    if (!recent) w *= 1.15f;
    if (midCap)
    {
        bool midRecent = false;
        for (size_t i = n > 2 ? n - 2 : 0; i < n; i++) midRecent = midRecent || MidHeavy(hist[i]);
        if (MidHeavy(c.p) && midRecent) w = 0.f;
        if (c.p.midShare >= 0.99f) w *= 0.5f;
    }
    return w;
}
// pick this round's plan (r in [0,1)); forceSite >= 0 = a human called that site (push or split there only);
// forceType >= 0 = he said "split" / "push"
inline TPlan PickPlan(const std::vector<PlanSite> &s, int nT, float smart, const std::vector<TPlan> &hist, bool midCap, float r,
                      int forceSite = -1, int forceType = -1)
{
    std::vector<PlanCand> c = PlanCands(s, nT, smart);
    std::vector<float> w;
    float tot = 0;
    for (auto &x : c)
    {
        float v = PlanWeight(x, hist, midCap);
        if (forceSite >= 0 && (x.p.site != forceSite || x.p.type == PL_FAKE)) v = 0;
        if (forceType >= 0 && x.p.type != forceType) v = 0;
        w.push_back(v);
        tot += v;
    }
    if (tot <= 0)
    {
        // nothing left (a forced type that site cannot play): a push at the forced site, else the first site
        TPlan p;
        p.site = forceSite >= 0 && forceSite < (int)s.size() ? forceSite : 0;
        p.ap1 = s.empty() ? 0 : MainAp(s[p.site]);
        p.midShare = !s.empty() && p.ap1 < (int)s[p.site].apMid.size() ? (float)s[p.site].apMid[p.ap1] : 0.f;
        return p;
    }
    float x = Clamp01(r) * tot;
    for (size_t i = 0; i < c.size(); i++)
    {
        if (x < w[i]) return c[i].p;
        x -= w[i];
    }
    for (size_t i = c.size(); i-- > 0;)
        if (w[i] > 0) return c[i].p;
    return c[0].p;
}
// a human's team-chat call: "a", "b", "go b", "b rush", "rush b", "b site", "b split", "a push", "lets go a" ...
// returns the site index (-1 = not a call); *type = PL_SPLIT / PL_PUSH when he said which, else -1. Anything with a
// word that is neither a site letter nor one of the call words is ordinary chat ("a bot is bad" is not a call).
inline int ParseCall(const std::string &text, const std::vector<std::string> &labels, int *type)
{
    static const char *filler[] = {"go", "rush", "push", "split", "site", "plz", "pls", "please", "now", "guys", "lets", "let's", "we", "all",
                                   "everyone", "team", "ok", "okay"};
    std::vector<std::string> words;
    std::string cur;
    for (char ch : text + " ")
    {
        char c = (char)tolower((unsigned char)ch);
        if (isalnum((unsigned char)c) || c == '\'') cur += c;
        else if (!cur.empty())
        {
            words.push_back(cur);
            cur.clear();
        }
    }
    if (type) *type = -1;
    if (words.empty() || words.size() > 4) return -1;
    int site = -1;
    for (auto &w : words)
    {
        int hit = -1;
        for (size_t i = 0; i < labels.size(); i++)
        {
            std::string l = labels[i];
            for (auto &ch : l) ch = (char)tolower((unsigned char)ch);
            if (w == l) hit = (int)i;
        }
        if (hit >= 0)
        {
            if (site >= 0 && site != hit) return -1;   // "a or b": not a call
            site = hit;
            continue;
        }
        bool fill = false;
        for (auto *f : filler) fill = fill || w == f;
        if (!fill) return -1;
        if (type && w == "split") *type = PL_SPLIT;
        if (type && (w == "push" || w == "rush")) *type = PL_PUSH;
    }
    return site;
}
// H: where is the bomb carrier heading? Each T approach is a line from the T spawn to its site's entry; his progress
// along one = how far along it he is while within `lat` of it. He is heading for site X once he is on X's site, or his
// progress on X's best route is >= max(700, 25% of it) and 500 more than on any other site's route.
struct Route
{
    int site;
    std::vector<V3> pts;
};
inline float RouteProgress(const Route &r, const V3 &pos, float lat, float *total)
{
    float run = 0, best = -1, bd = 1e9f;
    for (size_t i = 0; i + 1 < r.pts.size(); i++)
    {
        const V3 &a = r.pts[i], &b = r.pts[i + 1];
        float dx = b.x - a.x, dy = b.y - a.y, L = sqrtf(dx * dx + dy * dy);
        float t = L > 1e-3f ? ((pos.x - a.x) * dx + (pos.y - a.y) * dy) / (L * L) : 0.f;
        t = std::max(0.f, std::min(1.f, t));
        V3 q{a.x + t * dx, a.y + t * dy, a.z + t * (b.z - a.z)};
        float d = Dist2(q, pos);
        if (d < bd && fabsf(q.z - pos.z) < 200.f)
        {
            bd = d;
            best = run + t * L;
        }
        run += L;
    }
    if (total) *total = run;
    return bd <= lat ? best : -1.f;
}
inline int CarrierHeading(const std::vector<Route> &routes, const std::vector<V3> &siteC, const std::vector<float> &siteR, const V3 &pos, int *route = nullptr)
{
    int ns = (int)siteC.size();
    for (int i = 0; i < ns; i++)
        if (Dist2(pos, siteC[i]) < siteR[i] + 300.f && fabsf(pos.z - siteC[i].z) < 250.f)
        {
            if (route) *route = -1;
            return i;
        }
    std::vector<float> best(ns, -1.f), need(ns, 1e9f);
    std::vector<int> bestR(ns, -1);
    for (size_t k = 0; k < routes.size(); k++)
    {
        int s = routes[k].site;
        if (s < 0 || s >= ns) continue;
        float tot = 0, pr = RouteProgress(routes[k], pos, 380.f, &tot);
        if (pr > best[s])
        {
            best[s] = pr;
            bestR[s] = (int)k;
            need[s] = std::max(700.f, 0.25f * tot);
        }
    }
    int top = -1;
    for (int i = 0; i < ns; i++)
        if (best[i] >= 0 && (top < 0 || best[i] > best[top])) top = i;
    if (top < 0 || best[top] < need[top]) return -1;
    for (int i = 0; i < ns; i++)
        if (i != top && best[i] >= 0 && best[i] > best[top] - 500.f) return -1;
    if (route) *route = bestR[top];
    return top;
}

// ==== STEP 12E player habits: pre-aim, jiggle / shoulder peeks, counter-strafe =====================================
// His words: "they don't really try to jiggle peek, or do any player behaviors". By the bot's angles dial (0 = Silver I
// walks into sight .. 1 = Global Elite):
struct PeekParams
{
    float preaim;      // chance per look-decision (every ~0.4 s while moving on contested ground) to pre-aim the next angle
    float jiggle;      // chance to jiggle a suspected corner instead of walking into it
    int peeks;         // peeks per jiggle
    float out, back;   // seconds exposed per peek / back in cover between peeks
    float turn;        // pre-aim turn speed (deg/s)
};
inline PeekParams PeekFor(float d)
{
    d = Clamp01(d);
    PeekParams p;
    p.preaim = Lerp(0.2f, 0.95f, d);
    p.jiggle = d < 0.1f ? 0.f : Lerp(0.15f, 0.8f, (d - 0.1f) / 0.9f);
    p.peeks = d >= 0.6f ? 3 : 2;
    p.out = Lerp(0.35f, 0.12f, d);
    p.back = Lerp(0.8f, 0.4f, d);
    p.turn = Lerp(250.f, 700.f, d);
    return p;
}
// the peek point: the smallest sideways step (offsets in units, left and right) from which the corner can be seen
// (vis[i]) - none = no jiggle (the corner cannot be seen from beside it)
inline int PickPeek(const std::vector<float> &offsets, const std::vector<char> &vis)
{
    int best = -1;
    for (size_t i = 0; i < offsets.size() && i < vis.size(); i++)
        if (vis[i] && (best < 0 || fabsf(offsets[i]) < fabsf(offsets[best]))) best = (int)i;
    return best;
}
// the angle to pre-aim while moving: among spots ahead (within 75 deg of where it goes, 250..1300 u) that the enemy can
// already reach (reach[i]), not looked at in the last 4 s (fresh[i]) and in its sight (vis[i]), the nearest
inline int PickPreaim(const V3 &bot, float moveYaw, const std::vector<V3> &spots, const std::vector<char> &vis, const std::vector<char> &reach,
                      const std::vector<char> &fresh)
{
    int best = -1;
    float bd = 1e9f;
    for (size_t i = 0; i < spots.size(); i++)
    {
        if (i >= vis.size() || i >= reach.size() || i >= fresh.size() || !vis[i] || !reach[i] || !fresh[i]) continue;
        float d = Dist2(bot, spots[i]);
        if (d < 250.f || d > 1300.f || fabsf(Wrap180(YawTo(bot, spots[i]) - moveYaw)) > 75.f) continue;
        if (d < bd) { bd = d; best = (int)i; }
    }
    return best;
}

// ==== STEP 12K aim at Gold ==========================================================================================
// His words (09-26, Gold Nova I bots): "still not quite feeling like Gold", "not aiming right"; Gold should win real
// duels a fair share, not "walk up, headshot". Step 11 measured Gold (elo ~1100): sight -> first shot 0.64 s, first-shot
// hit 33%. A Gold human sees a still bot and lands a headshot in ~0.45-0.6 s. The retune lifts ONLY the middle ranks: a
// bump over the bot's aim dial that is 0 up to 0.10 (elo ~700: the kid floor's CT side is untouched), rises to 1 at 0.26
// (elo ~1000), stays 1 to 0.46 (~1300: Gold Nova I-III, Master Guardian I) and falls back to 0 at 0.74 (~1700); the top
// (step 11's calibration) and the bottom stay as they are, equal-elo matches stay equal (both sides move together).
inline float GoldBump(float aim)
{
    auto ss = [](float x) {
        x = Clamp01(x);
        return x * x * (3.f - 2.f * x);
    };
    if (!(aim > 0.10f) || aim >= 0.74f) return 0.f;
    if (aim < 0.26f) return ss((aim - 0.10f) / 0.16f);
    if (aim <= 0.46f) return 1.f;
    return ss((0.74f - aim) / 0.28f);
}
// what the full bump does to the profile values step 11 writes: ReactionTime x0.70, AimFocusInitial x0.60 (the first
// shot's aim error), AimFocusOffsetScale x0.70, LookAngleMaxAccelAttacking x1.35 (the flick); spray / Skill unchanged
struct GoldMul
{
    float rt, fi, fo, laa;
};
inline GoldMul GoldFor(float bump, float kRt = 0.30f, float kFi = 0.40f, float kFo = 0.30f, float kLa = 0.35f)
{
    bump = Clamp01(bump);
    return {1.f - kRt * bump, 1.f - kFi * bump, 1.f - kFo * bump, 1.f + kLa * bump};
}

// ==== STEP 12F the rest of his 09-25 list ===========================================================================
// F5 clearing a site on entry: the team's corners are shared out (bot i of n takes corners i, i+n, ...); each bot checks
// each of its corners with a chance by its awareness (40% .. 100%: "100% check corners" at the top); the team takes the
// site FAST (runs in, a quick glance per corner) or SLOW (walks in, holds each corner) - a smart team slow half the time,
// a sloppy one mostly fast ("fast sometimes, slow other times, scaled by rank")
inline std::vector<int> ClearShare(int nCorners, int nBots, int i)
{
    std::vector<int> v;
    if (nBots <= 0 || i < 0) return v;
    for (int k = i % nBots; k < nCorners; k += nBots) v.push_back(k);
    return v;
}
inline bool ClearSlow(float teamSmart, float r) { return r < 0.5f * Clamp01(teamSmart) + 0.05f; }
inline float ClearCheck(float aware) { return Lerp(0.4f, 1.f, Clamp01(aware)); }
inline float ClearHold(bool slow, float aware) { return slow ? Lerp(0.5f, 0.8f, Clamp01(aware)) : Lerp(0.2f, 0.3f, Clamp01(aware)); }
// F8 a retake goes in together: the first CT near the bomb (inside 900 u) waits for the next one when he is more than
// 600 u behind - once per retake, and never when the clock is tight (time left < the defuse + 5 s)
inline bool RetakeWait(float firstToBomb, float secondToBomb, float timeLeft, float defuse)
{
    return firstToBomb < 900.f && secondToBomb - firstToBomb > 600.f && timeLeft > defuse + 5.f;
}
// ... and hurries: the gather ends 4 s after the second CT is in, or 15 s after the plant, or when the clock says so
inline float RetakeDeadline(float plantAt, float walk, float defuse)
{
    return plantAt + std::min(15.f, std::max(4.f, 40.f - defuse - 3.f - walk));
}
// F9 a grenade's flight from the thrower's eye (CS:S / CS:GO lineage: the pitch is bent 10 deg up - level throws go 10 deg
// up - speed 675 u/s for a full throw, gravity 800 x 0.4): points every dt seconds up to tmax (the brain traces them)
inline std::vector<V3> ThrowArc(const V3 &eye, float pitch, float yaw, float dt = 0.05f, float tmax = 2.0f, float speed = 675.f, float g = 320.f)
{
    std::vector<V3> v;
    float p = pitch < 0 ? -10.f + pitch * (80.f / 90.f) : -10.f + pitch * (100.f / 90.f);
    float pr = p / 57.29578f, yr = yaw / 57.29578f;
    float vx = speed * cosf(pr) * cosf(yr), vy = speed * cosf(pr) * sinf(yr), vz = -speed * sinf(pr);
    V3 s{eye.x + 16.f * cosf(pr) * cosf(yr), eye.y + 16.f * cosf(pr) * sinf(yr), eye.z - 16.f * sinf(pr)};
    for (float t = 0; t <= tmax + 1e-4f; t += dt)
        v.push_back({s.x + vx * t, s.y + vy * t, s.z + vz * t - 0.5f * g * t * t});
    return v;
}
// "no molotov thrown the dumbest way": a fire / HE landing is dumb when it lands on a teammate (within 250 u), on the
// thrower (within 200 u: a wall in its face) or - a molotov - where no enemy is known within 900 u (and it is not on the
// bomb). Flags: 1 team, 2 self, 4 no target.
enum { ND_TEAM = 1, ND_SELF = 2, ND_NOTARGET = 4 };
inline int NadeDanger(const V3 &land, const V3 &thrower, const std::vector<V3> &mates, const std::vector<V3> &enemiesKnown, bool fire, bool nearBomb)
{
    int f = 0;
    for (auto &m : mates)
        if (Dist2(land, m) < 250.f && fabsf(land.z - m.z) < 150.f) f |= ND_TEAM;
    if (Dist2(land, thrower) < 200.f && fabsf(land.z - thrower.z) < 150.f) f |= ND_SELF;
    if (fire && !nearBomb)
    {
        bool target = false;
        for (auto &e : enemiesKnown) target = target || Dist(land, e) < 900.f;
        if (!target) f |= ND_NOTARGET;
    }
    return f;
}

// ==== STEP 12D team chat: recognising a one-line setter in machine code ============================================
// IServerGameClients::SetCommandClient(int index) is "g_nCommandClientIndex = index" (Source SDK). On the 32-bit Linux
// server.so (gcc, absolute addressing - step 10 found GetAllServerClasses as "push ebp; mov eax,[abs]; mov ebp,esp; pop
// ebp; ret") such a setter is: [push ebp; mov ebp,esp] load arg1 into eax/ecx/edx [pop ebp] store it to [abs]
// [pop ebp] ret. Returns true and the global's address when the bytes are exactly that (nothing else in between).
inline bool SetterPattern(const unsigned char *c, int n, unsigned *addr)
{
    int i = 0, reg = -1;
    bool frame = false, popped = false;
    auto has = [&](int k) { return i + k <= n; };
    if (has(3) && c[0] == 0x55 && c[1] == 0x89 && c[2] == 0xe5) { frame = true; i = 3; }
    // load: frame 8b 45|4d|55 0c ; no frame 8b 44|4c|54 24 08   (eax 0, ecx 1, edx 2)
    if (frame && has(3) && c[i] == 0x8b && c[i + 2] == 0x0c && (c[i + 1] == 0x45 || c[i + 1] == 0x4d || c[i + 1] == 0x55))
    {
        reg = c[i + 1] == 0x45 ? 0 : c[i + 1] == 0x4d ? 1 : 2;
        i += 3;
    }
    else if (!frame && has(4) && c[i] == 0x8b && c[i + 2] == 0x24 && c[i + 3] == 0x08 && (c[i + 1] == 0x44 || c[i + 1] == 0x4c || c[i + 1] == 0x54))
    {
        reg = c[i + 1] == 0x44 ? 0 : c[i + 1] == 0x4c ? 1 : 2;
        i += 4;
    }
    else
        return false;
    if (frame && has(1) && c[i] == 0x5d) { popped = true; i++; }
    // store: eax a3 imm32 ; ecx 89 0d imm32 ; edx 89 15 imm32
    unsigned a = 0;
    if (reg == 0 && has(5) && c[i] == 0xa3) { memcpy(&a, c + i + 1, 4); i += 5; }
    else if (reg > 0 && has(6) && c[i] == 0x89 && c[i + 1] == (reg == 1 ? 0x0d : 0x15)) { memcpy(&a, c + i + 2, 4); i += 6; }
    else return false;
    if (frame && !popped)
    {
        if (!has(1) || c[i] != 0x5d) return false;
        i++;
    }
    if (!has(1) || c[i] != 0xc3) return false;
    if (addr) *addr = a;
    return a >= 0x10000;
}
// the edict index a bot speaks with: every player's index (edict_t + 6, CS:GO's CBaseEdict m_EdictIndex) must be 1..64,
// unique, and the edicts must sit `stride` bytes apart in index order (one array) - the stride found, else 0
inline int EdictStride(const std::vector<std::pair<unsigned long, int>> &edictIndex)
{
    if (edictIndex.size() < 2) return 0;
    for (auto &e : edictIndex)
        if (e.second < 1 || e.second > 64) return 0;
    for (size_t i = 0; i < edictIndex.size(); i++)
        for (size_t j = i + 1; j < edictIndex.size(); j++)
            if (edictIndex[i].second == edictIndex[j].second) return 0;
    const auto &a = edictIndex[0];
    for (int stride : {16, 20, 24, 28, 32})
    {
        bool ok = true;
        for (auto &e : edictIndex)
            ok = ok && (long)(e.first - a.first) == (long)(e.second - a.second) * stride;
        if (ok) return stride;
    }
    return 0;
}

// "stood still eating bullets": in the 1.5 s after a hit, most speed samples under 40 u/s
inline bool StoodStill(const std::vector<float> &speeds)
{
    if (speeds.size() < 5) return false;
    int still = 0;
    for (float v : speeds) still += v < 40.f;
    return still >= 0.7f * speeds.size();
}

// ==== JOB 5b (ADDENDUM 2: his playtest of job 5, live 09-26) ========================================================
// ---- L: CT pushes rare and never aimed at the enemy -----------------------------------------------------------------
// His words: "a rush happens, but not every round... two people rushing cat one round, the next two rushing mid, then the
// next two rushing long... if they know where I'm going and they're sending two my way every time, that's super
// predictable." A push is now PLANNED in the freeze from the round and the team alone - never from where an enemy is,
// was heard or went last round: 10% + 12% x team smarts of rounds (a Silver team ~10%, Global Elite ~22%), never two
// rounds in a row, half as likely right after one; one CT (two in 30% of pushes, 4+ CTs) goes to a contested spot on
// the Ts' way in, holds it a few seconds and goes back; the spot is drawn over the map's push spots and is never one of
// the last two pushed.
struct PushPlan
{
    int target = -1, n = 0;
    float at = 0, hold = 0;   // seconds after the freeze; seconds held at the spot
};
inline float PushChance(float teamSmart, int roundsSinceLast)
{
    if (roundsSinceLast <= 1) return 0.f;
    float p = 0.10f + 0.12f * Clamp01(teamSmart);
    return roundsSinceLast == 2 ? 0.5f * p : p;
}
// recent = the targets of this map's earlier pushes, newest last
inline int PickPushTarget(int nTargets, const std::vector<int> &recent, float r)
{
    if (nTargets <= 0) return -1;
    std::vector<int> ok;
    int from = std::max(0, (int)recent.size() - 2);
    for (int i = 0; i < nTargets; i++)
    {
        bool rec = false;
        for (int k = from; k < (int)recent.size(); k++) rec = rec || recent[k] == i;
        if (!rec) ok.push_back(i);
    }
    if (ok.empty())
        for (int i = 0; i < nTargets; i++)
            if (recent.empty() || recent.back() != i) ok.push_back(i);
    if (ok.empty()) ok.push_back(0);
    int k = (int)(Clamp01(r) * ok.size());
    return ok[std::min(k, (int)ok.size() - 1)];
}
inline PushPlan PlanPush(float teamSmart, int roundsSinceLast, int nTargets, const std::vector<int> &recent, int nCT, float r1, float r2, float r3,
                         float r4)
{
    PushPlan p;
    if (nCT < 3 || nTargets <= 0 || r1 >= PushChance(teamSmart, roundsSinceLast)) return p;
    p.target = PickPushTarget(nTargets, recent, r2);
    p.n = nCT >= 4 && r3 < 0.3f ? 2 : 1;
    p.at = Lerp(2.f, 10.f, Clamp01(r4));
    p.hold = Lerp(2.5f, 5.f, Clamp01(r3));
    return p;
}
// a push spot: a point on a T approach that both teams can reach within 3 s of each other (nav earliest-occupy times,
// 1/10 s) - contested ground, not deep T ground; the better the closer the two times are
inline bool PushSpotOk(int occT, int occCT) { return occT < 1200 && occCT < 1200 && occCT <= occT + 30; }
inline int PushSpotScore(int occT, int occCT) { return occT > occCT ? occT - occCT : occCT - occT; }

// ---- M: the CT setup -------------------------------------------------------------------------------------------------
// His words: "defenders need to defend on site better. Maybe have one lurker and always have somebody watching mid";
// "there were four people watching mid"; "they really like to hold CT... one bot sat in CT for the last four rounds".
// CtRoles5b: the role of each CT slot - one ANCHOR on each site first (sites taken from `rot` on, so with fewer CTs than
// sites the open site changes round to round), then ONE mid (a map with a mid spot), then ONE lurker (a lurker spot and
// no stack), then the extra men: all to the stacked site, else the first to `extra`, the next to the emptiest site.
// Never a second mid, never a second lurker. 5 CTs on 2 sites: A, B, mid, lurker, + one on A or B (varies).
enum { CR_MID = -1, CR_LURK = -2 };
inline std::vector<int> CtRoles5b(int n, int nSites, bool hasMid, bool hasLurk, int stack, int extra, int rot)
{
    std::vector<int> r;
    if (n <= 0 || nSites <= 0) return r;
    if (stack >= nSites) stack = -1;
    if (extra < 0 || extra >= nSites) extra = 0;
    std::vector<int> cnt(nSites, 0);
    for (int i = 0; i < nSites && (int)r.size() < n; i++)
    {
        int s = ((i + rot) % nSites + nSites) % nSites;
        r.push_back(s);
        cnt[s]++;
    }
    if ((int)r.size() < n && hasMid) r.push_back(CR_MID);
    if ((int)r.size() < n && hasLurk && stack < 0) r.push_back(CR_LURK);
    bool first = true;
    while ((int)r.size() < n)
    {
        int s = stack;
        if (s < 0 && first) s = extra;
        else if (s < 0)
        {
            s = 0;
            for (int i = 1; i < nSites; i++)
                if (cnt[i] < cnt[s]) s = i;
        }
        first = false;
        r.push_back(s);
        cnt[s]++;
    }
    return r;
}
struct SpotCand
{
    V3 p;
    float q;   // quality 0..1 (a hold watching the entry 1, a covered hiding spot .75, a post spot .6, the rest .4)
};
// a CT's spot for this round: quality (weighted by its dial: a smart bot takes the good spots more often) + a random
// share; a spot used in the last rounds (by anyone) only when no other is free; never a spot taken this round (150 u)
inline int PickSpot(const std::vector<SpotCand> &c, const std::vector<V3> &taken, const std::vector<V3> &recent, float dial,
                    const std::vector<float> &rnd)
{
    int best = -1;
    float bs = -1e9f;
    for (size_t i = 0; i < c.size(); i++)
    {
        bool tk = false;
        for (auto &t : taken) tk = tk || Dist(c[i].p, t) < 150.f;
        if (tk) continue;
        float sc = Lerp(0.3f, 1.0f, Clamp01(dial)) * c[i].q + (i < rnd.size() ? rnd[i] : 0.5f) * 0.6f;
        for (auto &h : recent)
            if (Dist(c[i].p, h) < 120.f) sc -= 2.0f;
        if (sc > bs)
        {
            bs = sc;
            best = (int)i;
        }
    }
    return best;
}
struct Rect2
{
    float x0, y0, x1, y1, z;
};
// the flat distance from p to the rectangle (0 inside); far when the height differs by more than 200 u
inline float RectDist(const V3 &p, const Rect2 &r)
{
    if (fabsf(p.z - r.z) > 200.f) return 1e9f;
    float dx = p.x < r.x0 ? r.x0 - p.x : p.x > r.x1 ? p.x - r.x1 : 0.f;
    float dy = p.y < r.y0 ? r.y0 - p.y : p.y > r.y1 ? p.y - r.y1 : 0.f;
    return sqrtf(dx * dx + dy * dy);
}
// a site holder's spot: on the site (inside its radius), not in a mid area, not in the doorway (150 u from the entry),
// not by the CT spawn (650 u) unless it sits in the site's inner half
inline bool SiteSpotOk(const V3 &p, const V3 &siteC, float radius, const V3 &entry, const V3 &ctSpawn, bool inMid)
{
    float d = Dist2(p, siteC);
    if (d > radius || fabsf(p.z - siteC.z) > 350.f || inMid || Dist2(p, entry) < 150.f) return false;
    return !(Dist2(p, ctSpawn) < 650.f && d > 0.5f * radius);
}
// the mid holder's spot: CT ground (the CTs get there 2 s before the Ts), within 1500 u of a mid area, 650 u+ from the CT
// spawn, off every site (its radius + 150)
inline bool MidSpotOk(int occT, int occCT, float dMidArea, float dCtSpawn, float dSite, float siteRadius)
{
    return occCT < 1200 && occCT + 20 <= occT && dMidArea <= 1500.f && dCtSpawn >= 650.f && dSite > siteRadius + 150.f;
}
// the lurker's spot: CT ground, off every site (radius + 200), out of mid, 700 u+ from the CT spawn, 500-1600 u from a
// site's approach choke (it watches a way in from the CT side)
inline bool LurkSpotOk(int occT, int occCT, float dSite, float siteRadius, bool inMid, float dCtSpawn, float dChoke)
{
    return occCT < 1200 && occCT + 20 <= occT && dSite > siteRadius + 200.f && !inMid && dCtSpawn >= 700.f && dChoke >= 500.f && dChoke <= 1600.f;
}

// ---- N: rotations -----------------------------------------------------------------------------------------------------
// His words: "bomb planted on A, the bot on B not doing anything... finally starts rotating after the bomb's gonna blow
// up. It almost looks like he's only looking for exit frags... wanted to rotate through CT but then stopped because it saw
// me." Rotate on the call / plant at once: 1.0 .. 0.2 s by smarts + up to 0.4 s (step 9b: 3 .. 0.5 s + up to 1 s, and a
// sloppy CT sometimes stayed put; after the plant a CT that failed its roll was released to the stock AI).
inline float RotateDelay(float smart, float r) { return Lerp(1.0f, 0.2f, Clamp01(smart)) + 0.4f * Clamp01(r); }
// flat distance from p to the segment a-b
inline float SegDist2(const V3 &p, const V3 &a, const V3 &b)
{
    float vx = b.x - a.x, vy = b.y - a.y, wx = p.x - a.x, wy = p.y - a.y, L = vx * vx + vy * vy;
    float t = L > 1e-6f ? Clamp01((wx * vx + wy * vy) / L) : 0.f;
    float dx = wx - t * vx, dy = wy - t * vy;
    return sqrtf(dx * dx + dy * dy);
}
// sneaky when needed: an enemy the team knows about (seen or heard lately) within `r` of the way still ahead (the bot, the
// waypoints left, the goal) - then the game's safest route (it avoids where the team died) instead of the fastest
inline bool NeedSneaky(const std::vector<V3> &ahead, const std::vector<V3> &known, float r = 900.f)
{
    for (auto &k : known)
    {
        if (ahead.size() == 1 && Dist2(k, ahead[0]) < r) return true;
        for (size_t i = 0; i + 1 < ahead.size(); i++)
            if (SegDist2(k, ahead[i], ahead[i + 1]) < r && fabsf(k.z - ahead[i + 1].z) < 400.f) return true;
    }
    return false;
}

// ---- O: utility, every round ------------------------------------------------------------------------------------------
// His words: "We need flashes, smokes, mollies, grenades being used just about every single round by at least four out of
// five bots. If they can afford util they should be buying it like a normal player. If they have bought util they should
// be using it... not all at the same time."
enum UtilKind { UK_SMOKE, UK_FLASH, UK_HE, UK_FIRE, UK_DECOY, UK_N };
inline const char *UtilName(int k)
{
    static const char *n[] = {"smoke", "flash", "he", "fire", "decoy"};
    return k >= 0 && k < UK_N ? n[k] : "-";
}
inline int UtilPrice(int k, int team)
{
    switch (k)
    {
    case UK_SMOKE: return 300;
    case UK_FLASH: return 200;
    case UK_HE: return 300;
    case UK_FIRE: return team == 3 ? 600 : 400;   // CT incendiary / T molotov
    case UK_DECOY: return 50;
    }
    return 100000;
}
inline const char *UtilBuyName(int k, int team)
{
    static const char *n[] = {"smokegrenade", "flashbang", "hegrenade", "molotov", "decoy"};
    if (k == UK_FIRE && team == 3) return "incgrenade";
    return k >= 0 && k < UK_N ? n[k] : "";
}
// the kind of a grenade from a weapon / item / event name ("weapon_smokegrenade", "incgrenade", "molotov_projectile", ...)
inline int UtilKindOf(const char *w)
{
    if (!w) return -1;
    std::string l(w);
    for (auto &c : l) c = (char)tolower((unsigned char)c);
    const char *x = l.c_str();
    if (strstr(x, "smoke")) return UK_SMOKE;
    if (strstr(x, "flash")) return UK_FLASH;
    if (strstr(x, "hegrenade")) return UK_HE;
    if (strstr(x, "molotov") || strstr(x, "incgrenade") || strstr(x, "incendiary")) return UK_FIRE;
    if (strstr(x, "decoy")) return UK_DECOY;
    return -1;
}
// what a normal player buys with the money left after his gun and armour: smoke, flash, fire, a second flash, HE, in that
// order, each only when affordable; how much util in all by the bot's utility dial (1 at 0 .. 4 at 1: a Silver carries one
// or two, a Global Elite a full kit), counting what it already carries; the game's own limits (one of each, two flashes,
// four in all) still apply on top
inline std::vector<int> UtilBuyList(int money, int team, float dial, const int have[UK_N])
{
    static const int order[][2] = {{UK_SMOKE, 1}, {UK_FLASH, 1}, {UK_FIRE, 1}, {UK_FLASH, 2}, {UK_HE, 1}};   // kind, the count it brings it to
    // a carried flashbang pair is one weapon (have[] counts it once): a carried flash may be two - so the 4-grenade limit
    // counts it as two, and a second flash is only bought together with the first (never a rejected third)
    int want = 1 + (int)(Clamp01(dial) * 3.f + 0.5f), carry = 0, cnt[UK_N], pair = have[UK_FLASH] > 0 ? 1 : 0;
    for (int k = 0; k < UK_N; k++)
    {
        cnt[k] = have[k];
        if (k != UK_DECOY) carry += have[k];
    }
    std::vector<int> out;
    for (auto &o : order)
    {
        int k = o[0];
        if (carry >= want || carry + pair >= 4) break;
        if (cnt[k] >= o[1] || (k == UK_FLASH && o[1] == 2 && have[UK_FLASH] > 0)) continue;
        int pr = UtilPrice(k, team);
        if (money < pr) continue;
        money -= pr;
        cnt[k]++;
        carry++;
        out.push_back(k);
    }
    return out;
}
// the view pitches (Source: + = down) that land a throw at `target` in the ThrowArc model (level ground at the target's
// height, no walls - the brain traces the answer through the world): up to two - a flat throw and a lob, flat first
inline int ThrowPitches(const V3 &eye, const V3 &target, float out[2], float speed = 675.f, float g = 320.f)
{
    float D = Dist2(eye, target), dz = target.z - eye.z;
    int n = 0;
    float prevErr = 0;
    bool have = false;
    for (float vp = 89.f; vp >= -89.f; vp -= 0.5f)
    {
        float p = vp < 0 ? -10.f + vp * (80.f / 90.f) : -10.f + vp * (100.f / 90.f);
        float pr = p / 57.29578f, vh = speed * cosf(pr), vz = -speed * sinf(pr);
        float z0 = 16.f * -sinf(pr), x0 = 16.f * cosf(pr);   // the release point, 16 u ahead of the eye
        // descending root of z0 + vz t - g t^2 / 2 = dz
        float a = 0.5f * g, b = -vz, c = dz - z0, disc = b * b - 4 * a * c;
        if (disc < 0)
        {
            have = false;
            continue;
        }
        float t = (-b + sqrtf(disc)) / (2 * a);
        if (t <= 0)
        {
            have = false;
            continue;
        }
        float err = x0 + vh * t - D;
        if (have && ((err > 0) != (prevErr > 0)) && n < 2) out[n++] = vp + 0.5f * err / (err - prevErr);   // between this step and the last
        prevErr = err;
        have = true;
    }
    // the scan runs from looking down to looking up: the flat throw is found first
    return n;
}
// the view pitch whose flight is nearest `target` at `fuse` seconds (a flash / HE pops in the air), and that distance
inline float AirPitch(const V3 &eye, const V3 &target, float fuse, float *miss, float speed = 675.f, float g = 320.f)
{
    float best = 0, bd = 1e9f, yaw = YawTo(eye, target);
    for (float vp = 60.f; vp >= -80.f; vp -= 1.f)
    {
        std::vector<V3> a = ThrowArc(eye, vp, yaw, fuse, fuse, speed, g);
        float d = Dist(a.back(), target);
        if (d < bd)
        {
            bd = d;
            best = vp;
        }
    }
    if (miss) *miss = bd;
    return best;
}
// throws of one team are spread out ("not all at the same time"): the next one no sooner than this after the last
inline float UtilGap(int kind) { return kind == UK_FLASH ? 0.35f : kind == UK_SMOKE ? 0.8f : 0.7f; }
// how close a landing must come to count as a hit on the target (units)
inline float UtilTol(int kind) { return kind == UK_SMOKE ? 170.f : kind == UK_FIRE ? 150.f : kind == UK_FLASH ? 320.f : 220.f; }
struct UtilJob
{
    int bot, kind, slot;   // slot = the target index of that kind
    float delay;           // seconds after the plan
};
// the T execute (the group at the entrance): 1 smoke from a sloppy team, 2 from a smart one (one per smoke target), then
// one fire on a corner, each thrown by a different bot, spaced out; flashes come at the go-in (ExecuteFlashes)
inline std::vector<UtilJob> ExecuteJobs(const std::vector<std::vector<int>> &have, int nSmokeTargets, int nFireTargets, float smart)
{
    std::vector<UtilJob> out;
    std::vector<char> used(have.size(), 0);
    int smokes = std::min(nSmokeTargets, smart >= 0.4f ? 2 : 1);
    float t = 0.5f;
    for (int s = 0; s < smokes; s++)
        for (size_t i = 0; i < have.size(); i++)
            if (!used[i] && have[i][UK_SMOKE] > 0)
            {
                out.push_back({(int)i, UK_SMOKE, s, t});
                used[i] = 1;
                t += 0.8f;
                break;
            }
    if (nFireTargets > 0)
        for (size_t i = 0; i < have.size(); i++)
            if (!used[i] && have[i][UK_FIRE] > 0)
            {
                out.push_back({(int)i, UK_FIRE, 0, t});
                used[i] = 1;
                break;
            }
    return out;
}
// the flashes as the group goes in: up to 2 (1 from a sloppy team), bots that have one and threw nothing yet first
inline std::vector<int> ExecuteFlashes(const std::vector<std::vector<int>> &have, const std::vector<char> &threw, float smart)
{
    std::vector<int> out;
    int want = smart >= 0.3f ? 2 : 1;
    for (int pass = 0; pass < 2 && (int)out.size() < want; pass++)
        for (size_t i = 0; i < have.size() && (int)out.size() < want; i++)
            if (have[i][UK_FLASH] > 0 && (pass == 1 || !(i < threw.size() && threw[i])) &&
                std::find(out.begin(), out.end(), (int)i) == out.end())
                out.push_back((int)i);
    return out;
}
// a defender's pick when its site is under threat: the enemies still outside -> smoke the way in, then fire on it, then
// HE; inside / at the entry -> a flash, then HE, then fire (never a smoke onto its own site)
inline int DefendKind(const int have[UK_N], bool outside)
{
    static const int out[] = {UK_SMOKE, UK_FIRE, UK_HE}, in[] = {UK_FLASH, UK_HE, UK_FIRE};
    for (int k : outside ? out : in)
        if (have[k] > 0) return k;
    return -1;
}
// a retaker's pick on the way in: flash over the bomb, smoke off a post-plant angle, fire on a post spot, HE
inline int RetakeKind(const int have[UK_N])
{
    for (int k : {UK_FLASH, UK_SMOKE, UK_FIRE, UK_HE})
        if (have[k] > 0) return k;
    return -1;
}

// ---- P: movement in fights --------------------------------------------------------------------------------------------
// His words: "They stop moving too often... a normal person would move, stop, shoot, move, stop, shoot. They stop, shoot,
// and then stay stopped for as long as possible... They see me peek out long and stand still the whole time... They don't
// try to reposition after three people are looking at him... doing the same things every time, holding the same spots."
// A MOVER fight (a roll per fight by the footwork dial - step 10's plugin dial, 0 at elo 300, so a Silver keeps its
// planted spray: 0 .. 95%, ~45% for a Gold Nova) runs cycles: STOP (feet planted, the burst: 0.55 .. 0.30 s, x1.3 inside
// 800 u) then MOVE (the stock fight's own side-step, 0.35 .. 0.55 s), then STOP again at its next shot (after 0.15 s) or
// once the move is over and its crosshair is on him, 0.7 s at most.
inline bool Mover(float dial, float r) { return r < 0.95f * powf(Clamp01(dial), 0.7f); }
struct CycParams
{
    float burst, move, maxMove;
};
inline CycParams CycFor(float dial, float dist)
{
    float d = Clamp01(dial);
    return {Lerp(0.55f, 0.30f, d) * (dist < 800.f ? 1.3f : 1.f), Lerp(0.35f, 0.55f, d), 0.7f};
}
struct Cyc
{
    int phase = 0;   // 0 no fight, 1 STOP (shoot), 2 MOVE
    float at = 0, until = 0;
    int cycles = 0;
};
inline void CycStep(Cyc &c, bool fight, float t, bool onTarget, bool shot, const CycParams &p)
{
    if (!fight)
    {
        c.phase = 0;
        return;
    }
    if (c.phase == 0)
    {
        c = Cyc{1, t, t + p.burst, c.cycles};   // counter-strafe into the first burst
        return;
    }
    if (c.phase == 1 && t >= c.until)
    {
        c = Cyc{2, t, t + p.move, c.cycles + 1};
        return;
    }
    if (c.phase == 2 && ((shot && t - c.at >= 0.15f) || (t >= c.until && onTarget) || t - c.at >= p.maxMove))
        c = Cyc{1, t, t + p.burst, c.cycles};
}
// the side-step band for the MOVE phase (profile Skill: the stock fight side-steps above 0.5)
inline float MoveSkill(float dial) { return Lerp(0.55f, 0.70f, Clamp01(dial)); }
// after a fight a holder takes another spot (he knows where it is now): 20% .. 80% by its angles dial, x0.7 if it did not
// win the fight
inline bool RepoAfter(float dial, bool killed, float r) { return r < Lerp(0.2f, 0.8f, Clamp01(dial)) * (killed ? 1.f : 0.7f); }
// in the view cone of an enemy (his eye yaw, half-angle in degrees)
inline bool InCone(const V3 &eye, float yaw, const V3 &p, float half) { return fabsf(Wrap180(YawTo(eye, p) - yaw)) <= half; }
// seen by 2+ enemies at once where it stands: fall back to a spot none of them sees (hidden[i]), minD..maxD away, not
// towards them (their centre), cover preferred, the nearest such spot; -1 = none
inline int PickFallback(const V3 &bot, const std::vector<V3> &threats, const std::vector<V3> &cands, const std::vector<char> &hidden,
                        const std::vector<char> &cover, float minD, float maxD)
{
    if (threats.empty()) return -1;
    V3 c{0, 0, 0};
    for (auto &t : threats) c = {c.x + t.x, c.y + t.y, c.z + t.z};
    c = {c.x / threats.size(), c.y / threats.size(), c.z / threats.size()};
    float ax = c.x - bot.x, ay = c.y - bot.y, al = sqrtf(ax * ax + ay * ay);
    int best = -1;
    float bs = 1e9f;
    for (size_t i = 0; i < cands.size(); i++)
    {
        if (i >= hidden.size() || !hidden[i]) continue;
        float d = Dist2(bot, cands[i]);
        if (d < minD || d > maxD) continue;
        float dx = cands[i].x - bot.x, dy = cands[i].y - bot.y;
        if (al > 1.f && (dx * ax + dy * ay) / (al * std::max(1.f, d)) > 0.2f) continue;   // towards them
        float sc = d - (i < cover.size() && cover[i] ? 250.f : 0.f);
        if (sc < bs)
        {
            bs = sc;
            best = (int)i;
        }
    }
    return best;
}

// ---- Q: crouching (his follow-up: "we definitely want to make the bots crouch") -----------------------------------------
// The lever is the bot's own crouch flag (the one the bot turns into the duck button of its next command). No layout is
// trusted: in the bot's own part of the object the byte is FOUND by watching - the only byte that is only ever 0 / 1 and
// equals the player's FL_DUCKING whenever both have been steady for 0.75 s, on 2+ bots that ducked, 97%+ of the samples,
// 5 points clear of the next (CrouchPick) - and then PROVEN by effect before it is used (set on an idle bot, it must duck
// within 0.8 s; cleared, it must stand again).
struct CrouchCand
{
    int agreeD = 0, totalD = 0, agreeS = 0, totalS = 0;   // steady samples while ducked / standing, and how many it matched
    bool bad = false;                                     // was ever something else than 0 / 1
};
// the crouch flag among the candidate bytes: scored on the ducked and the standing samples apart (the worse of the two), so
// a byte that is always 0 never wins however rare ducking is; 97%+, 5 points clear of the next; -1 = none (never guess)
inline int CrouchPick(const std::vector<CrouchCand> &c, int ducked, int standing, int botsDucked)
{
    if (ducked < 12 || standing < 40 || botsDucked < 2) return -1;
    int best = -1;
    float br = 0, second = 0;
    for (size_t i = 0; i < c.size(); i++)
    {
        if (c[i].bad || c[i].totalD < 0.8f * ducked || c[i].totalS < 0.8f * standing) continue;
        float r = std::min((float)c[i].agreeD / c[i].totalD, (float)c[i].agreeS / c[i].totalS);
        if (r > br)
        {
            second = br;
            br = r;
            best = (int)i;
        }
        else if (r > second)
            second = r;
    }
    return best >= 0 && br >= 0.97f && br - second >= 0.05f ? best : -1;
}
// crouch-spray: a roll per fight at 350-1500 u by the footwork dial (step 10's plugin dial: 0 at elo 300, the kid floor),
// 0 .. 50% of fights (~30% for a Gold Nova): its bursts are fired crouched
inline bool CrouchSpray(float dial, float dist, float r) { return dist >= 350.f && dist <= 1500.f && r < 0.5f * sqrtf(Clamp01(dial)); }
// an angle held crouched (behind cover, an off-angle): a roll per hold spot by the angles dial (ramped like the footwork, 0
// at elo 300): 10 .. 40% of holds - only where the crouched eye still sees what it holds
inline bool CrouchHold(float dial, float r) { return dial > 0.f && r < Lerp(0.1f, 0.4f, Clamp01(dial)); }

// ==== JOB 5c (ADDENDUM 3: his playtest of job 5b, 09-29) ========================================================
// ---- Q: the CT setup, 2 - 2 - 1 -------------------------------------------------------------------------------------
// His words: "a defender puts two on A, two on B. One lingers. That lingerer usually watches mid. And then they play
// angles until somebody pushes into them, and then they take the shot... with every now and then a random aggressive push."
// CtRoles5c: one ANCHOR on each site first (from `extra` on), then the ONE lingerer (mid; the lurker spot when
// `lingerLurk`, or when the map has no mid spot), then the second men: the stacked site takes them all, else each goes to
// the emptiest site from `extra` on. 5 CTs on 2 sites: A, B, mid, A, B. 4 CTs: A, B, mid + one on `extra`.
inline std::vector<int> CtRoles5c(int n, int nSites, bool hasMid, bool hasLurk, int stack, bool lingerLurk, int extra)
{
    std::vector<int> r;
    if (n <= 0 || nSites <= 0) return r;
    if (stack >= nSites) stack = -1;
    if (extra < 0 || extra >= nSites) extra = 0;
    std::vector<int> cnt(nSites, 0);
    for (int i = 0; i < nSites && (int)r.size() < n; i++)
    {
        int s = (i + extra) % nSites;
        r.push_back(s);
        cnt[s]++;
    }
    if ((int)r.size() < n)
    {
        if (hasLurk && (lingerLurk || !hasMid)) r.push_back(CR_LURK);
        else if (hasMid) r.push_back(CR_MID);
    }
    while ((int)r.size() < n)
    {
        int s = stack;
        if (s < 0)
        {
            s = extra;
            for (int k = 0; k < nSites; k++)
                if (cnt[(extra + k) % nSites] < cnt[s]) s = (extra + k) % nSites;
        }
        r.push_back(s);
        cnt[s]++;
    }
    return r;
}
// Why 5b put them all at mid doors (measured in bot-only matches, elo 300, de_dust2): (1) part P's "fall back when 2+
// enemies see it" fired while the CTs crossed mid on their way out and swapped the CT's site spot for a hiding spot 300-900 u
// away - by mid doors - for the rest of the round; (2) the walk to the spot used the game's SAFEST route, which after a few
// deaths at mid sent a B holder round through A, long and the T side. Now a holder falls back / repositions only once it
// is AT its spot, and only to a spot on its own ground: its site (radius + 150) or 600 u round its mid / lurk spot.
inline bool CtAtSpot(float dToGoal, float arrive) { return dToGoal <= arrive + 150.f; }
inline bool CtOwnGround(bool siteRole, float dToSite, float siteRadius, float dToHome) { return siteRole ? dToSite <= siteRadius + 150.f : dToHome <= 600.f; }
// where a CT stands (the proof line): 0.. = on that site (radius + 150, within 350 u in height), -1 mid (within 400 u of a
// mid area), -2 at its lurk spot (400 u), -3 by the CT spawn (650 u), -4 on T ground, -5 anywhere else
enum { CA_MID = -1, CA_LURK = -2, CA_SPAWN = -3, CA_TSIDE = -4, CA_OTHER = -5 };
inline int CtArea(const V3 &p, const std::vector<V3> &siteC, const std::vector<float> &siteR, float dMid, float dLurkSpot, float dCtSpawn, bool tGround)
{
    for (size_t i = 0; i < siteC.size() && i < siteR.size(); i++)
        if (Dist2(p, siteC[i]) <= siteR[i] + 150.f && fabsf(p.z - siteC[i].z) < 350.f) return (int)i;
    if (dLurkSpot <= 400.f) return CA_LURK;
    if (dMid <= 400.f) return CA_MID;
    if (dCtSpawn < 650.f) return CA_SPAWN;
    return tGround ? CA_TSIDE : CA_OTHER;
}
// ---- R: a site holder that sees / hears a push fights it and calls it ----------------------------------------------
// His words: "we all push long. There's a bot that was on A who watched all five of us push and didn't do anything."
// on a site's way in: within `r` of the approach's line (its waypoints, stack, choke, entry), counting only the part of it
// within `near` of the site (the part by the T spawn is every approach's)
inline bool OnCorridor(const V3 &p, const std::vector<V3> &pts, const V3 &siteC, float near, float r)
{
    for (size_t i = 0; i + 1 < pts.size(); i++)
    {
        if (Dist2(pts[i], siteC) > near && Dist2(pts[i + 1], siteC) > near) continue;
        if (SegDist2(p, pts[i], pts[i + 1]) < r && fabsf(p.z - pts[i + 1].z) < 300.f) return true;
    }
    return false;
}
// a push = 2+ enemies on one way in (at least one of them SEEN by a holder, the rest seen or heard in the last 3 s)
inline bool PushSeen(int seen, int heard) { return seen >= 1 && seen + heard >= 2; }
// the hold that waited to be shot: K dropped it only for a QUIET enemy (none that fired in the last 1.5 s), so a team
// that pushed shooting was watched, never fought. Now the hold is dropped unless the enemy's shots are at THIS holder
// (then the stock hold fights back itself)
inline bool HoldDrop(bool enemyFiredLately, bool shotAtMe, bool reactOn) { return !enemyFiredLately || (reactOn && !shotAtMe); }
// a callout from an approach name (make_mapinfo: the nav place the way in runs through): "LongDoors" -> "long",
// "Catwalk" -> "cat", "UpperTunnel" / "Tunnels" -> "tunnels", "MidDoors" / "Middle" -> "mid", "TMain" -> "main",
// else the words of the name in lower case ("PalaceAlley" -> "palace alley"); "route0"-style names -> "" (none)
inline std::string CallName(const std::string &ap)
{
    if (ap == "LongDoors") return "long";
    if (ap == "Catwalk") return "cat";
    if (ap == "UpperTunnel" || ap == "Tunnels" || ap == "Tunnel") return "tunnels";
    if (ap == "MidDoors" || ap == "Middle" || ap == "SecondMid") return "mid";
    if (ap.compare(0, 5, "route") == 0) return "";
    std::string o;
    for (size_t i = 0; i < ap.size(); i++)
    {
        char c = ap[i];
        if (i == 0 && c == 'T' && ap.size() > 1 && isupper((unsigned char)ap[1])) continue;   // TMain, TRamp -> main, ramp
        if (isupper((unsigned char)c) && !o.empty() && o.back() != ' ') o += ' ';
        o += (char)tolower((unsigned char)c);
    }
    return o;
}
// ---- S: death info ----------------------------------------------------------------------------------------------------
// His words: "If a person dies or a bot dies on a team, it should actively give the other bots information... where I died
// and who killed me... or the minimap." Each living teammate takes the info in by rank: a Silver 55% of deaths, 1.4-1.8 s
// late; a Global Elite every death, 0.3-0.7 s late (the minimap glance)
struct DeathReact
{
    bool takes;
    float delay;
};
inline DeathReact DeathFor(float smart, float r1, float r2)
{
    smart = Clamp01(smart);
    return {r1 < Lerp(0.55f, 1.0f, smart), Lerp(1.4f, 0.3f, smart) + 0.4f * Clamp01(r2)};
}
// what the defenders do with it before the plant: 2+ enemies known there = that site is hit (the rotations go), one = a
// look / one helper (part C's level 1), none on a site's way in = only the angles turn
inline int DeathCallLevel(int enemiesThere, bool onSiteWay) { return !onSiteWay ? 0 : enemiesThere >= 2 ? 2 : 1; }
// the line a teammate says: "Ian died long, 2 there" (his "died long, 2 there"); no place = "near A"
inline std::string DeathText(const std::string &victim, const std::string &place, int enemiesThere)
{
    std::string t = victim + " died " + place;
    if (enemiesThere >= 2) t += ", " + std::to_string(enemiesThere) + " there";
    return t;
}
// ---- U: defenders face the way in they hold ----------------------------------------------------------
// His words: "looking weird directions, walking real slow."
// the angle a holder faces: among the ways in it can SEE from its spot (vis), one the Ts reach earliest (occT, 1/10 s) -
// any of those within 3 s of the earliest, by the draw r (so the angle varies); -1 = it sees none (the stock look)
inline int FacePick(const std::vector<char> &vis, const std::vector<int> &occT, float r)
{
    int best = 1 << 30;
    for (size_t i = 0; i < vis.size() && i < occT.size(); i++)
        if (vis[i]) best = std::min(best, occT[i]);
    if (best == 1 << 30) return -1;
    std::vector<int> ok;
    for (size_t i = 0; i < vis.size() && i < occT.size(); i++)
        if (vis[i] && occT[i] <= best + 30) ok.push_back((int)i);
    return ok[std::min((int)ok.size() - 1, (int)(Clamp01(r) * ok.size()))];
}
}   // namespace fl
