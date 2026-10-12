// ======================================================================
// AmunNeuro.cpp
// ======================================================================
#include "Amun/Components/AmunNeuro/AmunNeuro.hpp"

#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>
#include <unistd.h>
#include <csignal>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace Amun {

namespace {
// ---- PIKA pins (BCM) ----
constexpr U32 PIN_CE0 = 8, PIN_RESET_B = 26, PIN_CFGFLG_B = 23, PIN_ACTIVATE = 24, PIN_ERR_B = 25;
constexpr U32 PIN_LCL_ACLK_EN = 14, PIN_ACLK_SEL0 = 5, PIN_ACLK_SEL1 = 6;
constexpr U32 COMP_PIN[4] = {16, 18, 20, 12};  // FPAA0..3 IO5P (Pi.Ka quick start)
// ---- BCM2711 GPIO registers (/dev/gpiomem word offsets) ----
constexpr U32 GPFSEL0 = 0x00 / 4, GPSET0 = 0x1C / 4, GPCLR0 = 0x28 / 4, GPLEV0 = 0x34 / 4;
// ---- decoded configuration map (from AD2 probe exports) ----
struct Addr { U8 bank; U8 byte; };
constexpr Addr V1_ADDR[4] = {{4, 29}, {3, 25}, {3, 25}, {3, 25}};
constexpr Addr V2_ADDR[4] = {{3, 29}, {4, 17}, {4, 17}, {4, 17}};
constexpr Addr W_ADDR[4]  = {{3, 5}, {3, 2}, {3, 2}, {3, 2}};   // Cf, C_V2, C_V1
constexpr U8 PLUS = 0x51, MINUS = 0x41;
// ---- timing ----
constexpr U32 DEFAULT_SPI_HZ = 32000000, SPI_CHUNK = 64;
constexpr U32 RESET_LOW_US = 20000, RESET_SETTLE_US = 100000, AUTONULL_US = 60000;
// ---- monitor parameters (units: ms of +1 input at w = 0.1) ----
constexpr F32 BIAS = 10.0f;      // neurons start at -BIAS: fire after > BIAS units of net evidence
constexpr F32 EST_MIN = -18.0f;  // rail guard (rail ~ +-45 units)
constexpr F32 EST_MAX = 18.0f;
constexpr U32 TMAX_US = 8000;    // full-strength evidence per 1 Hz sample (> worst hold drift)
constexpr F32 W_CRUISE = 0.1f, W_LANDING = 0.2f;
const char* const FEATURE[4] = {"fix", "satellites", "HDOP", "position-jump"};
const char* const CHECKPOINT = "/home/amun/f/neuro.chk";
const char* const HEARTBEAT = "/home/amun/f/neuro.hb";       // liveness time only (no evidence)
const char* const TRUTH_LOG = "/home/amun/f/est_truth.log";  // evaluation ground truth, never read by F'
const char* const WEIGHTS_FILE = "/home/amun/f/weights.txt";
constexpr F32 RAIL_UNITS = 44.0f;     // |state| beyond this = saturated
constexpr F32 DIS_MARGIN = 3.0f;      // disagreement only counted when |estimate| > margin
constexpr U8 DIS_STEPS = 2;           // consecutive disagreeing samples before repair
constexpr U32 STEP_MS = 100;          // episode step period (TRAIN / INFER)
constexpr U32 DEADLINE = 6;           // detection deadline, steps after fault onset
constexpr U32 GRACE = 4;              // steps after a fault ends before a fire counts as false alarm
constexpr F32 W_MIN = 0.02f, W_MAX = 0.5f;
// ---- v7 autonomy (sources in AMUN_v7_spec.md section 7) ----
const char* const CAL_FILE = "/home/amun/f/neuro.cal";
const char* const LEARNED_FILE = "/home/amun/f/learned.txt";
const char* const WARM_FLAG = "/home/amun/f/warm.flag";
const char* const SAFE_FLAG = "/home/amun/f/safe.flag";
const char* const BOOTS_LOG = "/home/amun/f/boots.log";
const char* const FAULT_CFG = "/home/amun/f/fault.cfg";
const char* const AMUN_CFG = "/home/amun/f/amun.cfg";
constexpr F32 V0_CLOCK = 18.0f;       // clock neuron recharge level (units)
constexpr F32 WATCHDOG_S = 3.0f;      // supervisor watchdog W (mission choice; must match amun_supervisor.sh)
constexpr F32 MEM_LIMIT_KB = 65536.0f;// memory margin before the (simulated) OOM kill
constexpr F32 GPS_FAIL_S = 3.0f;      // GPS silence that counts as link loss
constexpr U32 BASE_TICKS = 30;        // quiet baseline length
constexpr U32 P_WARN = 2;             // persistence before SHED
constexpr F32 H_PROBE_S = 10.0f;      // shed duration before the probe resume
constexpr U32 P_PROBE = 3;            // probe window (ticks)
constexpr F32 PROBE_HIT = 0.5f;       // evidence during probe that confirms the fault
constexpr F32 ETA = 0.15f, BETA = 2.0f;
constexpr F32 FLOOR_MAX = -4.0f;      // floor can never come closer to threshold than this
constexpr F32 D_REQ_S = 10.0f;        // downtime the evidence must survive
constexpr F32 ATTRIB = 0.5f;          // fraction of the way from floor to threshold = attributable
constexpr U32 B1_K = 3;               // boot-counter baseline: K deaths ...
constexpr F32 B1_T_S = 120.0f;        // ... within T_K seconds
const char* const MON_NAME[4] = {"TIMING", "MEMORY", "GPS-LINK", "CLOCK"};

void sleepUs(U32 us) {
    struct timespec ts;
    ts.tv_sec = us / 1000000;
    ts.tv_nsec = static_cast<long>(us % 1000000) * 1000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}
F32 clampf(F32 v, F32 lo, F32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

// weight -> (Cf, Cin) 8-bit capacitor codes; largest Cf with best ratio
void caps(F32 wa, F32 wb, U8& cf, U8& ca, U8& cb) {
    F64 best = 1e9;
    cf = 250; ca = 25; cb = 25;
    for (int f = 255; f >= 1; --f) {
        const long a = std::lround(wa * f), b = std::lround(wb * f);
        if (a < 1 || a > 255 || b < 1 || b > 255) continue;
        const F64 e = std::fabs(static_cast<F64>(a) / f - wa) + std::fabs(static_cast<F64>(b) / f - wb);
        if (e < best - 1e-12) {
            best = e; cf = static_cast<U8>(f); ca = static_cast<U8>(a); cb = static_cast<U8>(b);
        }
        if (e == 0.0) break;
    }
}
// NMEA "ddmm.mmmm" -> degrees
F64 nmeaDeg(const char* s, const char* hemi) {
    if (s == nullptr || *s == '\0') return 0.0;
    const F64 v = std::atof(s);
    const F64 deg = std::floor(v / 100.0);
    F64 d = deg + (v - deg * 100.0) / 60.0;
    if (hemi != nullptr && (*hemi == 'S' || *hemi == 'W')) d = -d;
    return d;
}
}  // namespace

// ======================================================================
AmunNeuro::AmunNeuro(const char* const compName)
    : AmunNeuroComponentBase(compName), m_primaryLen(0), m_gpio(nullptr), m_gpioFd(-1), m_spiFd(-1),
      m_spiHz(DEFAULT_SPI_HZ), m_booted(false), m_errOk(true), m_frames(0), m_monitor(false),
      m_resetOnMode(false), m_mode(Amun::NeuroMode::CRUISE), m_gpsTrusted(true), m_ttyFd(-1), m_lineLen(0),
      m_fix(0), m_sats(0), m_hdop(99.99f), m_haveFix(false), m_lat(0), m_lon(0), m_prevLat(0), m_prevLon(0),
      m_havePrev(false), m_speedMps(0), m_jump(false), m_calibrated(false), m_lastStepNs(0), m_chargeRtNs(0), m_ckRtNs(0), m_ckPeriod(0), m_lastCkNs(0),
      m_scrubPeriod(0), m_lastScrubNs(0), m_scrubs(0), m_repair(Amun::RepairMode::NONE), m_nSteps(0),
      m_auto(false), m_warmChecked(false), m_baseLeft(0), m_bN(0),
      m_runaway(false), m_runDelta(0), m_runN(0), m_burstLeft(0), m_burstMs(0), m_leakOn(false), m_leakKB(0),
      m_cacheLeft(0), m_cacheKB(0), m_gpsBlock(false), m_nBlocks(0), m_rssPrev(0), m_rssBase(0),
      m_prevTickNs(0), m_lastNmeaNs(nowNs()), m_policy(0), m_ckEvery(0), m_learn(false), m_auditEvery(0), m_clock(true),
      m_w0(0.1f), m_b0(-18.0f), m_lastAuditNs(0), m_auditIdx(0), m_lastPolCkNs(0), m_safe(false) {
    m_safeCause[0] = '\0';
    for (U32 i = 0; i < N_MON; i++) m_nOf[i] = static_cast<U8>(i);
    for (U32 i = 0; i < 3; i++) {
        m_off[i] = 0; m_scale[i] = 1; m_wMon[i] = 0.1f; m_bSum[i] = 0; m_bSq[i] = 0;
        m_fsm[i] = S_NOM; m_fsmCnt[i] = 0; m_shedNs[i] = 0; m_probing[i] = false; m_probeLeft[i] = 0; m_probeHit[i] = false;
    }
    m_shed[0] = m_shed[1] = false;
    for (U32 i = 0; i < MAX_BLOCKS; i++) { m_blocks[i] = nullptr; m_blkCache[i] = false; }
    for (U32 k = 0; k < 4; k++) {
        m_v1[k] = PLUS; m_v2[k] = MINUS; m_pos[k] = false; m_w[k] = W_CRUISE;
        m_est[k] = 0; m_fired[k] = false; m_injPct[k] = 0; m_injLeft[k] = 0; m_chargedUs[k] = 0; m_leak[k] = 0;
        m_ckEst[k] = 0; m_mismatch[k] = 0; m_wLearned[k] = 0.1f; m_floor[k] = EST_MIN;
    }
}

AmunNeuro::~AmunNeuro() {
    this->hwClose();
    if (m_ttyFd >= 0) ::close(m_ttyFd);
}

// ======================================================================
// hardware layer
// ======================================================================
U64 AmunNeuro::nowNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<U64>(ts.tv_sec) * 1000000000ULL + static_cast<U64>(ts.tv_nsec);
}
U64 AmunNeuro::nowRtNs() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<U64>(ts.tv_sec) * 1000000000ULL + static_cast<U64>(ts.tv_nsec);
}
void AmunNeuro::pinMode(U32 pin, bool output) {
    const U32 reg = GPFSEL0 + pin / 10, shift = (pin % 10) * 3;
    U32 v = m_gpio[reg];
    v &= ~(7U << shift);
    if (output) v |= (1U << shift);
    m_gpio[reg] = v;
}
void AmunNeuro::pinWrite(U32 pin, bool high) { m_gpio[high ? GPSET0 : GPCLR0] = (1U << pin); }
bool AmunNeuro::pinRead(U32 pin) const { return (m_gpio[GPLEV0] >> pin) & 1U; }

bool AmunNeuro::hwOpen(U32 spiHz, bool keepRunning) {
    this->hwClose();
    m_spiHz = (spiHz == 0) ? DEFAULT_SPI_HZ : spiHz;
    m_gpioFd = ::open("/dev/gpiomem", O_RDWR | O_SYNC);
    if (m_gpioFd < 0) { this->log_WARNING_HI_HwError(Fw::LogStringArg("open /dev/gpiomem"), errno); return false; }
    void* map = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, m_gpioFd, 0);
    if (map == MAP_FAILED) {
        this->log_WARNING_HI_HwError(Fw::LogStringArg("mmap gpiomem"), errno);
        ::close(m_gpioFd); m_gpioFd = -1; return false;
    }
    m_gpio = static_cast<volatile U32*>(map);
    const U32 outs[] = {PIN_RESET_B, PIN_CE0, PIN_LCL_ACLK_EN, PIN_ACLK_SEL0, PIN_ACLK_SEL1};
    if (keepRunning) {
        // REATTACH: keep RESET_B high and the oscillator on -> no reset, analog state untouched
        this->pinWrite(PIN_RESET_B, true); this->pinWrite(PIN_LCL_ACLK_EN, true);
        this->pinWrite(PIN_CE0, false); this->pinWrite(PIN_ACLK_SEL0, false); this->pinWrite(PIN_ACLK_SEL1, false);
        for (U32 p : outs) this->pinMode(p, true);
    } else {
        for (U32 p : outs) { this->pinWrite(p, false); this->pinMode(p, true); }
    }
    const U32 ins[] = {PIN_CFGFLG_B, PIN_ACTIVATE, PIN_ERR_B, COMP_PIN[0], COMP_PIN[1], COMP_PIN[2], COMP_PIN[3]};
    for (U32 p : ins) this->pinMode(p, false);
    this->pinWrite(PIN_LCL_ACLK_EN, true);
    this->pinWrite(PIN_CE0, false);
    m_spiFd = ::open("/dev/spidev0.0", O_RDWR);
    if (m_spiFd < 0) { this->log_WARNING_HI_HwError(Fw::LogStringArg("open /dev/spidev0.0"), errno); this->hwClose(); return false; }
    U32 mode32 = SPI_MODE_0 | SPI_NO_CS;
    if (ioctl(m_spiFd, SPI_IOC_WR_MODE32, &mode32) < 0) {
        U8 mode8 = static_cast<U8>(SPI_MODE_0 | SPI_NO_CS);
        if (ioctl(m_spiFd, SPI_IOC_WR_MODE, &mode8) < 0) { mode8 = SPI_MODE_0; (void)ioctl(m_spiFd, SPI_IOC_WR_MODE, &mode8); }
    }
    U8 bits = 8, lsb = 0;
    (void)ioctl(m_spiFd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    (void)ioctl(m_spiFd, SPI_IOC_WR_LSB_FIRST, &lsb);
    if (ioctl(m_spiFd, SPI_IOC_WR_MAX_SPEED_HZ, &m_spiHz) < 0) {
        this->log_WARNING_HI_HwError(Fw::LogStringArg("set SPI speed"), errno); this->hwClose(); return false;
    }
    return true;
}
void AmunNeuro::hwClose() {
    if (m_spiFd >= 0) { ::close(m_spiFd); m_spiFd = -1; }
    if (m_gpio != nullptr) { munmap(const_cast<U32*>(m_gpio), 4096); m_gpio = nullptr; }
    if (m_gpioFd >= 0) { ::close(m_gpioFd); m_gpioFd = -1; }
    m_booted = false;
}
bool AmunNeuro::spiWrite(const U8* data, U32 len) {
    for (U32 off = 0; off < len; off += SPI_CHUNK) {
        const U32 n = (len - off < SPI_CHUNK) ? (len - off) : SPI_CHUNK;
        struct spi_ioc_transfer tr;
        std::memset(&tr, 0, sizeof(tr));
        tr.tx_buf = reinterpret_cast<unsigned long>(data + off);
        tr.len = n; tr.speed_hz = m_spiHz; tr.bits_per_word = 8;
        if (ioctl(m_spiFd, SPI_IOC_MESSAGE(1), &tr) < 0) {
            this->log_WARNING_HI_HwError(Fw::LogStringArg("SPI transfer"), errno); return false;
        }
    }
    return true;
}
// one data block: D5 addr 80 | (byte|0x80) bank n data... 2A | 00 dummy
bool AmunNeuro::sendFrame(U8 chip, U8 bank, U8 byte, const U8* data, U8 n) {
    U8 f[16];
    U32 i = 0;
    f[i++] = 0xD5; f[i++] = chip; f[i++] = 0x80;
    f[i++] = static_cast<U8>(0x80 | (byte & 0x3F)); f[i++] = bank; f[i++] = n;
    for (U8 j = 0; j < n; j++) f[i++] = data[j];
    f[i++] = 0x2A; f[i++] = 0x00;
    const bool ok = this->spiWrite(f, i) && this->pinRead(PIN_ERR_B);
    m_frames++;
    if (!ok) m_errOk = false;
    return ok;
}
bool AmunNeuro::primary() {
    this->pinWrite(PIN_RESET_B, false); sleepUs(RESET_LOW_US);
    this->pinWrite(PIN_RESET_B, true); sleepUs(RESET_SETTLE_US);
    const bool sent = this->spiWrite(m_primary, m_primaryLen);
    sleepUs(AUTONULL_US);
    for (U32 k = 0; k < 4; k++) { m_v1[k] = PLUS; m_v2[k] = MINUS; m_w[k] = 0.1f; }
    return sent && this->pinRead(PIN_ERR_B) && this->pinRead(PIN_ACTIVATE);
}

// ======================================================================
// neuron primitives
// ======================================================================
bool AmunNeuro::comp(U32 k) const { return this->pinRead(COMP_PIN[k]); }
bool AmunNeuro::positive(U32 k) const { return this->comp(k) == m_pos[k]; }

void AmunNeuro::setInput(U32 k, I32 x) {
    const U8 w1 = (x < 0) ? MINUS : PLUS;    // +1:(+,+)  0:(+,-)  -1:(-,-)
    const U8 w2 = (x > 0) ? PLUS : MINUS;
    if (m_v1[k] != w1) { this->sendFrame(static_cast<U8>(k + 1), V1_ADDR[k].bank, V1_ADDR[k].byte, &w1, 1); m_v1[k] = w1; }
    if (m_v2[k] != w2) { this->sendFrame(static_cast<U8>(k + 1), V2_ADDR[k].bank, V2_ADDR[k].byte, &w2, 1); m_v2[k] = w2; }
}
void AmunNeuro::setWeights(U32 k, F32 wV1, F32 wV2) {
    U8 cf, cV2, cV1;
    caps(wV2, wV1, cf, cV2, cV1);
    const U8 d[3] = {cf, cV2, cV1};
    this->sendFrame(static_cast<U8>(k + 1), W_ADDR[k].bank, W_ADDR[k].byte, d, 3);
    m_w[k] = wV1;
    this->log_ACTIVITY_LO_WeightsSet(static_cast<U8>(k + 1), cf, cV2, cV1);
}
// RUN+ in 5 ms steps (holding between) until each comparator changes; that level = positive
void AmunNeuro::calibrate() {
    bool c0[4], done[4] = {false, false, false, false};
    for (U32 k = 0; k < 4; k++) c0[k] = this->comp(k);
    for (U32 step = 0; step < 30; step++) {
        for (U32 k = 0; k < 4; k++) if (!done[k]) this->setInput(k, +1);
        sleepUs(5000);
        for (U32 k = 0; k < 4; k++) this->setInput(k, 0);
        sleepUs(2000);
        bool all = true;
        for (U32 k = 0; k < 4; k++) {
            if (!done[k] && this->comp(k) != c0[k]) { m_pos[k] = this->comp(k); done[k] = true; }
            all = all && done[k];
        }
        if (all) return;
    }
    for (U32 k = 0; k < 4; k++) if (!done[k]) m_pos[k] = c0[k];
}
// drive every membrane to the comparator threshold, then hold.
// Short drive (60 ms ~ 4 V at w=0.1, more than the full linear range); if a neuron does not
// cross, try the opposite direction once (never pushes a membrane into the rail for long).
bool AmunNeuro::zeroAll(U32 timeoutMs) { return this->zeroMask(0x0F, timeoutMs); }
bool AmunNeuro::zeroMask(U8 mask, U32 timeoutMs) {
    const U8 orig = mask;
    bool ok = true;
    // full rail-to-threshold travel at the slowest weight in the mask, with 60 % margin
    F32 wMin = 1.0f;
    for (U32 k = 0; k < 4; k++) if ((mask & (1U << k)) && m_w[k] < wMin) wMin = m_w[k];
    const U32 needMs = static_cast<U32>(1.6f * 45.0f * (0.1f / wMin));
    if (timeoutMs < needMs) timeoutMs = needMs;
    for (U32 attempt = 0; attempt < 2; attempt++) {
        bool start[4], left[4];
        U32 n = 0;
        for (U32 k = 0; k < 4; k++) {
            left[k] = false; start[k] = this->comp(k);
            if (!(mask & (1U << k))) continue;
            left[k] = true; n++;
            I32 dir = (start[k] == m_pos[k]) ? -1 : +1;
            if (attempt == 1 && !m_calibrated) dir = -dir;   // polarity unknown: try the other way
            if (attempt == 1 && m_calibrated) { m_v1[k] = 0; m_v2[k] = 0; }   // shadow suspect: force both bytes
            this->setInput(k, dir);
        }
        const U64 t0 = nowNs();
        while (n > 0 && (nowNs() - t0) < static_cast<U64>(timeoutMs) * 1000000ULL) {
            for (U32 k = 0; k < 4; k++)
                if (left[k] && this->comp(k) != start[k]) { this->setInput(k, 0); left[k] = false; n--; }
        }
        for (U32 k = 0; k < 4; k++) if (left[k]) this->setInput(k, 0);
        if (n == 0) { for (U32 k = 0; k < 4; k++) if (orig & (1U << k)) m_est[k] = 0; return ok; }
        ok = false;
        for (U32 k = 0; k < 4; k++)
            if (left[k]) this->log_WARNING_HI_HwError(Fw::LogStringArg("zero: neuron did not cross threshold"), static_cast<I32>(k + 1));
        if (attempt == 0) { for (U32 k = 0; k < 4; k++) if (!left[k]) mask = static_cast<U8>(mask & ~(1U << k)); }
    }
    // still failed: do not pretend the chip is at threshold. Estimate = where it is (measured or rail)
    { F32 st[4]; bool rail[4]; this->readState(mask, st, rail); }   // readState leaves m_est = 0 (crossed) or +-45 (rail)
    for (U32 k = 0; k < 4; k++) if ((orig & ~mask) & (1U << k)) m_est[k] = 0;
    return ok;
}
// run several neurons' inputs concurrently, each for its own duration (C++ timed)
void AmunNeuro::pulseAll(const I32 x[4], const U32 us[4]) {
    U64 tOn[4];
    bool on[4];
    for (U32 k = 0; k < 4; k++) {
        on[k] = (x[k] != 0 && us[k] > 0);
        if (on[k]) { this->setInput(k, x[k]); tOn[k] = nowNs(); }
    }
    bool any = true;
    while (any) {
        any = false;
        for (U32 k = 0; k < 4; k++) {
            if (!on[k]) continue;
            const U64 el = nowNs() - tOn[k];
            if (el >= static_cast<U64>(us[k]) * 1000ULL) {
                this->setInput(k, 0); on[k] = false;
                m_est[k] += static_cast<F32>(x[k]) * (static_cast<F32>(el) / 1e6f) * (m_w[k] / 0.1f);
            } else {
                any = true;
            }
        }
    }
}
// hold drift per neuron: charge +1 for 20 ms, hold 3 s, return and time the crossing.
// drift = (T - t_x) / hold seconds (units of +1 ms at w = 0.1 per s; + = state decays downward).
void AmunNeuro::measureLeak() {
    U64 ta[4], tb[4], ts[4]; bool left[4];
    for (U32 k = 0; k < 4; k++) { this->setInput(k, +1); ta[k] = nowNs(); }
    const U64 tEnd = ta[0] + 20000000ULL;
    while (nowNs() < tEnd) { }
    for (U32 k = 0; k < 4; k++) { this->setInput(k, 0); tb[k] = nowNs(); }
    sleepUs(3000000);
    for (U32 k = 0; k < 4; k++) { this->setInput(k, -1); ts[k] = nowNs(); left[k] = true; }
    U32 n = 4; const U64 t0 = nowNs();
    while (n > 0 && nowNs() - t0 < 100000000ULL) {
        for (U32 k = 0; k < 4; k++)
            if (left[k] && !this->positive(k)) {
                const F32 tx = static_cast<F32>(nowNs() - ts[k]) / 1e6f, T = static_cast<F32>(tb[k] - ta[k]) / 1e6f;
                this->setInput(k, 0); left[k] = false; n--;
                const F32 holdS = static_cast<F32>(ts[k] - tb[k]) / 1e9f;
                m_leak[k] = (T - tx) / holdS;
                this->log_ACTIVITY_HI_LeakMeasured(static_cast<U8>(k + 1), m_leak[k]);
            }
    }
    for (U32 k = 0; k < 4; k++) if (left[k]) { this->setInput(k, 0); m_leak[k] = 0; this->log_WARNING_HI_HwError(Fw::LogStringArg("leak measurement: no crossing"), static_cast<I32>(k + 1)); }
}
// boot image -> calibrate -> zero -> weights for mode -> bias to -BIAS
void AmunNeuro::bootNeurons() {
    (void)this->primary();
    if (!m_calibrated) {
        this->calibrate(); m_calibrated = true;
        (void)this->primary(); (void)this->zeroAll(60);
        this->measureLeak();
        (void)this->primary();
    }
    (void)this->zeroAll(60);
    const F32 w = (m_mode == Amun::NeuroMode::LANDING) ? W_LANDING : W_CRUISE;
    for (U32 k = 0; k < 4; k++) {
        if (std::fabs(m_w[k] - w) > 1e-6f) this->setWeights(k, w, w);
        m_est[k] = 0; m_fired[k] = false;
    }
    I32 x[4] = {-1, -1, -1, -1};
    U32 us[4];
    for (U32 k = 0; k < 4; k++) us[k] = static_cast<U32>(BIAS * 1000.0f * (0.1f / m_w[k]));
    this->pulseAll(x, us);
    m_lastStepNs = nowNs();      // drift compensation starts from the moment of biasing
    m_booted = true;
}

// ======================================================================
// GPS (u-blox NMEA on /dev/ttyACM0)
// ======================================================================
void AmunNeuro::gpsPoll() {
    if (m_ttyFd < 0) {
        m_ttyFd = ::open("/dev/ttyACM0", O_RDONLY | O_NONBLOCK | O_NOCTTY);
        if (m_ttyFd < 0) return;
        struct termios t;
        if (tcgetattr(m_ttyFd, &t) == 0) {
            cfmakeraw(&t); cfsetispeed(&t, B9600); tcsetattr(m_ttyFd, TCSANOW, &t);
        }
    }
    char buf[512];
    for (;;) {
        const ssize_t n = ::read(m_ttyFd, buf, sizeof(buf));
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            const char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (m_lineLen > 0) { m_line[m_lineLen] = '\0'; this->gpsLine(m_line); m_lineLen = 0; }
            } else if (m_lineLen < sizeof(m_line) - 1) {
                m_line[m_lineLen++] = c;
            }
        }
    }
}
void AmunNeuro::gpsLine(const char* s) {
    if (s[0] != '$' || std::strlen(s) < 7) return;
    m_lastNmeaNs = nowNs();
    char tmp[128];
    std::strncpy(tmp, s, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = '\0';
    char* star = std::strchr(tmp, '*');
    if (star) *star = '\0';
    const char* f[20];
    U32 nf = 0;
    char* p = tmp;
    f[nf++] = p;
    while (*p && nf < 20) { if (*p == ',') { *p = '\0'; f[nf++] = p + 1; } p++; }
    const char* id = f[0] + 3;   // skip "$GP" / "$GN"
    if (std::strncmp(id, "GGA", 3) == 0 && nf >= 9) {
        m_fix = static_cast<U8>(std::atoi(f[6]));
        m_sats = static_cast<U8>(std::atoi(f[7]));
        m_hdop = (*f[8] != '\0') ? static_cast<F32>(std::atof(f[8])) : 99.99f;
        if (m_fix > 0 && *f[2] != '\0') {
            const F64 lat = nmeaDeg(f[2], f[3]), lon = nmeaDeg(f[4], f[5]);
            if (m_havePrev) {
                const F64 dy = (lat - m_prevLat) * 111320.0;
                const F64 dx = (lon - m_prevLon) * 111320.0 * std::cos(lat * 3.14159265358979 / 180.0);
                const F64 d = std::sqrt(dx * dx + dy * dy);
                m_jump = d > (50.0 + 3.0 * m_speedMps);   // implausible 1 s displacement
            }
            m_prevLat = lat; m_prevLon = lon; m_havePrev = true; m_haveFix = true;
        } else {
            m_haveFix = false; m_havePrev = false; m_jump = false;
        }
    } else if (std::strncmp(id, "RMC", 3) == 0 && nf >= 8) {
        m_speedMps = (*f[7] != '\0') ? static_cast<F32>(std::atof(f[7]) * 0.514444) : 0.0f;
    }
}
// one 1 Hz monitor step: features -> evidence -> pulses -> decisions -> F' action
void AmunNeuro::monitorStep() {
    F32 e[4];
    e[0] = (m_fix == 0) ? 1.0f : -1.0f;
    e[1] = clampf((6.0f - static_cast<F32>(m_sats)) / 4.0f, -1.0f, 1.0f);
    e[2] = clampf((m_hdop - 2.0f) / 3.0f, -1.0f, 1.0f);
    e[3] = m_jump ? 1.0f : -1.0f;
    const U64 tNow = nowNs();
    F32 dt = (m_lastStepNs == 0) ? 1.0f : static_cast<F32>(tNow - m_lastStepNs) / 1e9f;
    if (dt > 30.0f) dt = 30.0f;
    m_lastStepNs = tNow;
    for (U32 k = 0; k < 4; k++) m_est[k] = clampf(m_est[k] - m_leak[k] * dt, -45.0f, 45.0f);   // measured hold drift
    for (U32 k = 0; k < 4; k++)
        if (m_injLeft[k] > 0) { e[k] = static_cast<F32>(m_injPct[k]) / 100.0f; m_injLeft[k]--; }
    this->applyEvidence(e);
    U8 mask = 0;
    for (U32 k = 0; k < 4; k++) {
        const bool f = this->positive(k);
        if (f && !m_fired[k]) this->log_WARNING_HI_GpsDegraded(static_cast<U8>(k + 1), Fw::LogStringArg(FEATURE[k]));
        if (!f && m_fired[k]) this->log_ACTIVITY_HI_GpsRecovered(static_cast<U8>(k + 1), Fw::LogStringArg(FEATURE[k]));
        m_fired[k] = f;
        if (f) mask |= static_cast<U8>(1U << k);
    }
    const bool trusted = (mask == 0);
    if (trusted != m_gpsTrusted) { m_gpsTrusted = trusted; this->log_ACTIVITY_HI_GpsTrust(trusted); }
    this->tlmWrite_FiredMask(mask);
    this->tlmWrite_GpsTrusted(m_gpsTrusted);
    this->writeTruth(mask);
    // E8b: periodic software checkpoint (the baseline the analog state competes with)
    if (m_ckPeriod > 0 && (m_lastCkNs == 0 || tNow - m_lastCkNs >= static_cast<U64>(m_ckPeriod) * 1000000000ULL)) {
        this->saveCheckpoint(); m_lastCkNs = tNow;
    }
    // E9: periodic blind scrub (state-preserving rewrite of the golden configuration)
    if (m_scrubPeriod > 0 && (m_lastScrubNs == 0 || tNow - m_lastScrubNs >= static_cast<U64>(m_scrubPeriod) * 1000000000ULL)) {
        for (U32 k = 0; k < 4; k++) this->scrubNeuron(k);
        m_scrubs++; m_lastScrubNs = tNow;
        this->log_ACTIVITY_LO_Scrubbed(m_scrubs);
    }
    // E9: software estimate vs analog comparator
    this->checkDisagree();
}

// ternary evidence pulses for one step, with rail guard and floor maintenance (shared by MONITOR / TRAIN / INFER)
void AmunNeuro::applyEvidence(const F32 e[4]) {
    I32 x[4];
    U32 us[4];
    for (U32 k = 0; k < 4; k++) {
        x[k] = (e[k] > 0.05f) ? 1 : ((e[k] < -0.05f) ? -1 : 0);
        F32 dur = std::fabs(e[k]) * static_cast<F32>(TMAX_US);
        const F32 unitsPerUs = (m_w[k] / 0.1f) / 1000.0f;
        const F32 room = (x[k] > 0) ? (EST_MAX - m_est[k]) : (m_est[k] - m_floor[k]);
        if (room <= 0.0f) dur = 0.0f;
        else if (dur * unitsPerUs > room) dur = room / unitsPerUs;
        if (x[k] <= 0 && m_est[k] < m_floor[k]) {       // floor maintenance: never let drift reach the rail
            x[k] = +1; dur = (m_floor[k] - m_est[k]) / unitsPerUs;
        }
        us[k] = static_cast<U32>(dur);
    }
    this->pulseAll(x, us);
}

// ======================================================================
// handlers
// ======================================================================
void AmunNeuro::run_handler(FwIndexType portNum, U32 context) {
    (void)portNum; (void)context;
    if (!m_warmChecked) { m_warmChecked = true; if (!m_booted) (void)this->warmStart(); }
    if (!m_gpsBlock) this->gpsPoll();
    this->tlmWrite_GpsFix(m_fix);
    this->tlmWrite_GpsSats(m_sats);
    this->tlmWrite_GpsHdop(m_hdop);
    if (this->ready()) {
        if (m_auto) this->autoStep();
        else if (m_monitor) this->monitorStep();
        this->tlmWrite_Est1(m_est[0]); this->tlmWrite_Est2(m_est[1]);
        this->tlmWrite_Est3(m_est[2]); this->tlmWrite_Est4(m_est[3]);
        this->tlmWrite_FramesSent(m_frames);
        this->tlmWrite_ErrOk(m_errOk);
    }
}

bool AmunNeuro::loadImage(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (f == nullptr) { this->log_WARNING_HI_HwError(Fw::LogStringArg("open boot image"), errno); return false; }
    U32 n = 0; char line[64]; bool bad = false;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        char* p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '\0') continue;
        unsigned int v = 0;
        if (std::sscanf(p, "%x", &v) != 1 || v > 0xFF || n >= MAX_PRIMARY) { bad = true; break; }
        m_primary[n++] = static_cast<U8>(v);
    }
    std::fclose(f);
    if (bad || n == 0) { this->log_WARNING_LO_BadArg(Fw::LogStringArg("boot image parse")); return false; }
    m_primaryLen = n;
    return true;
}
void AmunNeuro::saveCheckpoint() {
    FILE* f = std::fopen(CHECKPOINT, "w");
    if (f == nullptr) return;
    for (U32 k = 0; k < 4; k++)
        std::fprintf(f, "%d %u %.6f %.6f %.6f\n", m_pos[k] ? 1 : 0, m_chargedUs[k], static_cast<double>(m_leak[k]),
                     static_cast<double>(m_w[k]), static_cast<double>(m_est[k]));
    std::fprintf(f, "%llu %llu\n", static_cast<unsigned long long>(m_chargeRtNs), static_cast<unsigned long long>(nowRtNs()));
    std::fclose(f);
}
bool AmunNeuro::loadCheckpoint() {
    FILE* f = std::fopen(CHECKPOINT, "r");
    if (f == nullptr) return false;
    bool ok = true;
    for (U32 k = 0; k < 4; k++) {
        int p = 0; unsigned int c = 0; double lk = 0, w = 0.1, es = 0;
        if (std::fscanf(f, "%d %u %lf %lf %lf", &p, &c, &lk, &w, &es) != 5) { ok = false; break; }
        m_pos[k] = (p != 0); m_chargedUs[k] = c; m_leak[k] = static_cast<F32>(lk); m_w[k] = static_cast<F32>(w);
        m_ckEst[k] = static_cast<F32>(es);
    }
    unsigned long long t = 0, te = 0;
    if (ok && std::fscanf(f, "%llu %llu", &t, &te) != 2) ok = false;
    m_chargeRtNs = static_cast<U64>(t); m_ckRtNs = static_cast<U64>(te);
    std::fclose(f);
    return ok;
}

void AmunNeuro::BOOT_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, const Fw::CmdStringArg& path, U32 spiHz) {
    std::remove(HEARTBEAT);                // long blocking command: disarm the external watchdog
    std::remove(WARM_FLAG);                // BOOT = fresh start; AUTONOMY re-arms warm starts
    m_auto = false;
    if (!this->loadImage(path.toChar())) { this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR); return; }
    if (!this->hwOpen(spiHz)) { this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR); return; }
    m_errOk = true; m_frames = 0; m_calibrated = false;
    if (!this->primary()) {
        this->log_WARNING_HI_HwError(Fw::LogStringArg("primary configuration (ERR_B/ACTIVATE)"), 0);
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR); return;
    }
    this->bootNeurons();
    U8 mask = 0;
    for (U32 k = 0; k < 4; k++) if (m_pos[k]) mask |= static_cast<U8>(1U << k);
    for (U32 k = 0; k < 4; k++) m_chargedUs[k] = 0;
    this->saveCheckpoint();
    this->log_ACTIVITY_HI_Booted(mask, m_frames);
    this->cmdResponse_out(opCode, cmdSeq, m_errOk ? Fw::CmdResponse::OK : Fw::CmdResponse::EXECUTION_ERROR);
}

void AmunNeuro::SET_INPUT_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U8 neuron, I8 x) {
    if (!this->ready()) { this->log_WARNING_LO_NotReady(); this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR); return; }
    if (neuron < 1 || neuron > 4 || x < -1 || x > 1) { this->log_WARNING_LO_BadArg(Fw::LogStringArg("neuron 1..4, x -1..1")); this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::VALIDATION_ERROR); return; }
    this->setInput(neuron - 1U, x);
    this->cmdResponse_out(opCode, cmdSeq, m_errOk ? Fw::CmdResponse::OK : Fw::CmdResponse::EXECUTION_ERROR);
}

void AmunNeuro::PULSE_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U8 neuron, I8 x, U32 us) {
    if (!this->ready()) { this->log_WARNING_LO_NotReady(); this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR); return; }
    if (neuron < 1 || neuron > 4 || x < -1 || x > 1 || us > 100000) { this->log_WARNING_LO_BadArg(Fw::LogStringArg("neuron 1..4, x -1..1, us<=100000")); this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::VALIDATION_ERROR); return; }
    I32 xs[4] = {0, 0, 0, 0}; U32 d[4] = {0, 0, 0, 0};
    xs[neuron - 1] = x; d[neuron - 1] = us;
    this->pulseAll(xs, d);
    this->cmdResponse_out(opCode, cmdSeq, m_errOk ? Fw::CmdResponse::OK : Fw::CmdResponse::EXECUTION_ERROR);
}

void AmunNeuro::SET_WEIGHTS_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U8 neuron, F32 wV1, F32 wV2) {
    if (!this->ready()) { this->log_WARNING_LO_NotReady(); this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR); return; }
    if (neuron > 4 || wV1 < 0.01f || wV1 > 3.92f || wV2 < 0.01f || wV2 > 3.92f) { this->log_WARNING_LO_BadArg(Fw::LogStringArg("neuron 0..4, w 0.01..3.92")); this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::VALIDATION_ERROR); return; }
    for (U32 k = 0; k < 4; k++) if (neuron == 0 || neuron == k + 1) this->setWeights(k, wV1, wV2);
    this->cmdResponse_out(opCode, cmdSeq, m_errOk ? Fw::CmdResponse::OK : Fw::CmdResponse::EXECUTION_ERROR);
}

void AmunNeuro::CARRY_TRIAL_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U16 tMs, U32 holdMs, F32 wNew, bool restore) {
    std::remove(HEARTBEAT);                // long blocking command: disarm the external watchdog
    if (!this->ready()) { this->log_WARNING_LO_NotReady(); this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR); return; }
    if (tMs < 2 || tMs > 30 || holdMs > 60000 || wNew < 0.03f || wNew > 1.0f) { this->log_WARNING_LO_BadArg(Fw::LogStringArg("tMs 2..30, holdMs<=60000, wNew .03..1")); this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::VALIDATION_ERROR); return; }
    const bool wasMon = m_monitor; m_monitor = false;
    (void)this->primary();
    (void)this->zeroAll(60);
    U64 ta[4], tb[4];
    for (U32 k = 0; k < 4; k++) { this->setInput(k, +1); ta[k] = nowNs(); }
    const U64 tEnd = ta[0] + static_cast<U64>(tMs) * 1000000ULL;
    while (nowNs() < tEnd) { }
    for (U32 k = 0; k < 4; k++) { this->setInput(k, 0); tb[k] = nowNs(); }
    sleepUs(holdMs * 500U);
    for (U32 k = 0; k < 4; k++) this->setWeights(k, wNew, wNew);           // rewrite while the state is held
    if (restore) { sleepUs(1000); for (U32 k = 0; k < 4; k++) this->setWeights(k, 0.1f, 0.1f); }
    sleepUs(holdMs * 500U);
    U64 ts[4]; bool left[4];
    for (U32 k = 0; k < 4; k++) { this->setInput(k, -1); ts[k] = nowNs(); left[k] = true; }
    U32 n = 4;
    const U64 t0 = nowNs();
    while (n > 0 && nowNs() - t0 < 300000000ULL) {
        for (U32 k = 0; k < 4; k++) {
            if (left[k] && !this->positive(k)) {
                const U64 tx = nowNs() - ts[k];
                this->setInput(k, 0); left[k] = false; n--;
                const U32 txUs = static_cast<U32>(tx / 1000ULL), tRunUs = static_cast<U32>((tb[k] - ta[k]) / 1000ULL);
                this->log_ACTIVITY_HI_TrialResult(static_cast<U8>(k + 1), restore ? 0.1f : wNew, tRunUs, txUs,
                                                  static_cast<F32>(txUs) / static_cast<F32>(tRunUs));
            }
        }
    }
    for (U32 k = 0; k < 4; k++) if (left[k]) { this->setInput(k, 0); this->log_WARNING_HI_TrialNoCross(static_cast<U8>(k + 1)); }
    this->bootNeurons();          // back to the monitoring baseline
    m_monitor = wasMon;
    this->cmdResponse_out(opCode, cmdSeq, m_errOk ? Fw::CmdResponse::OK : Fw::CmdResponse::EXECUTION_ERROR);
}

void AmunNeuro::MONITOR_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, bool enable) {
    if (!enable) std::remove(HEARTBEAT);   // heartbeat stops on purpose: disarm the external watchdog
    if (enable && this->ready() && (m_lastStepNs == 0 || nowNs() - m_lastStepNs > 5000000000ULL)) {
        (void)this->zeroAll(0);            // stale estimate: re-establish a known state
        F32 b[4] = {-BIAS, -BIAS, -BIAS, -BIAS};
        this->chargeTo(0x0F, b);
        for (U32 k = 0; k < 4; k++) m_fired[k] = false;
        m_lastStepNs = nowNs();
    }
    if (enable && !this->ready()) { this->log_WARNING_LO_NotReady(); this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR); return; }
    m_monitor = enable;
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void AmunNeuro::MODE_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, const Amun::NeuroMode& mode) {
    if (!this->ready()) { this->log_WARNING_LO_NotReady(); this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR); return; }
    m_mode = mode;
    const F32 w = (mode == Amun::NeuroMode::LANDING) ? W_LANDING : W_CRUISE;
    if (m_resetOnMode) {
        this->bootNeurons();                         // baseline: evidence wiped
    } else {
        for (U32 k = 0; k < 4; k++) this->setWeights(k, w, w);   // carry: evidence kept
    }
    this->log_ACTIVITY_HI_ModeChanged(mode, m_resetOnMode);
    this->cmdResponse_out(opCode, cmdSeq, m_errOk ? Fw::CmdResponse::OK : Fw::CmdResponse::EXECUTION_ERROR);
}

void AmunNeuro::RESET_ON_MODE_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, bool enable) {
    m_resetOnMode = enable;
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void AmunNeuro::INJECT_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U8 neuron, I8 pct, U16 seconds) {
    if (neuron < 1 || neuron > 4 || pct < -100 || pct > 100) { this->log_WARNING_LO_BadArg(Fw::LogStringArg("neuron 1..4, pct -100..100")); this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::VALIDATION_ERROR); return; }
    m_injPct[neuron - 1] = pct; m_injLeft[neuron - 1] = seconds;
    this->log_WARNING_LO_Injected(neuron, pct, seconds);
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void AmunNeuro::CLOSE_cmdHandler(FwOpcodeType opCode, U32 cmdSeq) {
    m_monitor = false;
    this->hwClose();
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void AmunNeuro::CHARGE_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U16 tMs) {
    if (!this->ready()) { this->log_WARNING_LO_NotReady(); this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR); return; }
    if (tMs < 2 || tMs > 30) { this->log_WARNING_LO_BadArg(Fw::LogStringArg("tMs 2..30")); this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::VALIDATION_ERROR); return; }
    m_monitor = false;
    (void)this->primary();
    (void)this->zeroAll(60);
    U64 ta[4];
    for (U32 k = 0; k < 4; k++) { this->setInput(k, +1); ta[k] = nowNs(); }
    const U64 tEnd = ta[0] + static_cast<U64>(tMs) * 1000000ULL;
    while (nowNs() < tEnd) { }
    for (U32 k = 0; k < 4; k++) { this->setInput(k, 0); m_chargedUs[k] = static_cast<U32>((nowNs() - ta[k]) / 1000ULL); }
    m_chargeRtNs = nowRtNs();
    for (U32 k = 0; k < 4; k++) m_est[k] = static_cast<F32>(m_chargedUs[k]) / 1000.0f;
    m_lastStepNs = nowNs();
    this->saveCheckpoint();
    this->log_ACTIVITY_HI_Charged(tMs);
    this->cmdResponse_out(opCode, cmdSeq, m_errOk ? Fw::CmdResponse::OK : Fw::CmdResponse::EXECUTION_ERROR);
}

void AmunNeuro::READOUT_cmdHandler(FwOpcodeType opCode, U32 cmdSeq) {
    if (!this->ready()) { this->log_WARNING_LO_NotReady(); this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR); return; }
    m_monitor = false;
    const F32 heldS = (m_chargeRtNs > 0) ? static_cast<F32>(nowRtNs() - m_chargeRtNs) / 1e9f : 0.0f;
    // above threshold: drive -1 until it falls (state > 0); below: drive +1 until it rises (state < 0)
    U64 ts[4]; bool left[4]; I32 dir[4];
    for (U32 k = 0; k < 4; k++) {
        dir[k] = this->positive(k) ? -1 : +1;
        this->setInput(k, dir[k]); ts[k] = nowNs(); left[k] = true;
    }
    U32 n = 4;
    const U64 t0 = nowNs();
    while (n > 0 && nowNs() - t0 < 300000000ULL) {
        for (U32 k = 0; k < 4; k++) {
            if (left[k] && (this->positive(k) == (dir[k] > 0))) {
                const F32 txMs = static_cast<F32>(nowNs() - ts[k]) / 1e6f;
                this->setInput(k, 0); left[k] = false; n--;
                const F32 scale = m_w[k] / 0.1f;                       // units = ms of +1 at w = 0.1
                const F32 state = static_cast<F32>(-dir[k]) * txMs * scale;
                const F32 charged = static_cast<F32>(m_chargedUs[k]) / 1000.0f * scale;
                const F32 drift = m_leak[k];                          // weight-independent
                const F32 recovered = state + drift * heldS;           // undo the measured drift
                const F32 rRaw = (charged > 0) ? state / charged : 0.0f;
                const F32 rCorr = (charged > 0) ? recovered / charged : 0.0f;
                if (std::fabs(state) > 40.0f) this->log_WARNING_HI_ReadoutRail(static_cast<U8>(k + 1), state);
                this->log_ACTIVITY_HI_Readout(static_cast<U8>(k + 1), state, charged, heldS, drift, rRaw, rCorr);
            }
        }
    }
    for (U32 k = 0; k < 4; k++) if (left[k]) { this->setInput(k, 0); this->log_WARNING_HI_ReadoutNoCross(static_cast<U8>(k + 1)); }
    this->cmdResponse_out(opCode, cmdSeq, m_errOk ? Fw::CmdResponse::OK : Fw::CmdResponse::EXECUTION_ERROR);
}

void AmunNeuro::CRASH_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, bool inRun) {
    (void)opCode; (void)cmdSeq;
    this->log_WARNING_HI_Crashing(inRun);
    if (inRun && this->ready()) {
        for (U32 k = 0; k < 4; k++) this->setInput(k, +1);   // die in the middle of a +1 input
    }
    ::raise(SIGKILL);                                         // same as kill -9: no cleanup, no response
}

void AmunNeuro::REATTACH_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, const Fw::CmdStringArg& path, bool writeHold) {
    if (!this->loadImage(path.toChar())) { this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR); return; }
    if (!this->hwOpen(0, true)) { this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR); return; }
    m_errOk = true; m_frames = 0;
    const bool ck = this->loadCheckpoint();       // polarity, charge, drift, weights, charge time
    m_calibrated = ck;
    for (U32 k = 0; k < 4; k++) {
        if (writeHold) {                           // identical HOLD bytes rewritten (explicit writes, no reset)
            const U8 p = PLUS, m = MINUS;
            this->sendFrame(static_cast<U8>(k + 1), V1_ADDR[k].bank, V1_ADDR[k].byte, &p, 1);
            this->sendFrame(static_cast<U8>(k + 1), V2_ADDR[k].bank, V2_ADDR[k].byte, &m, 1);
        }                                          // else: zero configuration writes (valid after a crash in HOLD)
        m_v1[k] = PLUS; m_v2[k] = MINUS;
        m_fired[k] = this->positive(k);
    }
    m_booted = true;
    U8 mask = 0, above = 0;
    for (U32 k = 0; k < 4; k++) {
        if (m_pos[k]) mask |= static_cast<U8>(1U << k);
        if (m_fired[k]) above |= static_cast<U8>(1U << k);
    }
    const F32 heldS = (ck && m_chargeRtNs > 0) ? static_cast<F32>(nowRtNs() - m_chargeRtNs) / 1e9f : 0.0f;
    if (!ck) this->log_WARNING_LO_BadArg(Fw::LogStringArg("no checkpoint: polarity unknown"));
    this->log_ACTIVITY_HI_Reattached(mask, heldS, writeHold, above);
    this->cmdResponse_out(opCode, cmdSeq, (m_errOk && ck) ? Fw::CmdResponse::OK : Fw::CmdResponse::EXECUTION_ERROR);
}

// ======================================================================
// E8b / E9 helpers
// ======================================================================
// time each membrane back to threshold (destructive). st = signed state in units (ms of +1 at w=0.1)
void AmunNeuro::readState(U8 mask, F32 st[4], bool rail[4]) {
    U64 ts[4]; bool left[4]; I32 dir[4];
    U32 n = 0;
    for (U32 k = 0; k < 4; k++) {
        st[k] = 0; rail[k] = false; left[k] = false; dir[k] = 0;
        if (!(mask & (1U << k))) continue;
        dir[k] = this->positive(k) ? -1 : +1;
        this->setInput(k, dir[k]); ts[k] = nowNs(); left[k] = true; n++;
    }
    F32 wMin = 1.0f;
    for (U32 k = 0; k < 4; k++) if ((mask & (1U << k)) && m_w[k] < wMin) wMin = m_w[k];
    const U64 limitNs = static_cast<U64>(1.6f * 45.0f * (0.1f / wMin) * 1e6f);
    const U64 t0 = nowNs();
    while (n > 0 && nowNs() - t0 < limitNs) {
        for (U32 k = 0; k < 4; k++) {
            if (left[k] && (this->positive(k) == (dir[k] > 0))) {
                const F32 txMs = static_cast<F32>(nowNs() - ts[k]) / 1e6f;
                this->setInput(k, 0); left[k] = false; n--;
                st[k] = static_cast<F32>(-dir[k]) * txMs * (m_w[k] / 0.1f);
            }
        }
    }
    for (U32 k = 0; k < 4; k++) {
        if (left[k]) { this->setInput(k, 0); st[k] = static_cast<F32>(-dir[k]) * 99.0f; }
        if ((mask & (1U << k)) && std::fabs(st[k]) > RAIL_UNITS) rail[k] = true;
        if (mask & (1U << k)) m_est[k] = left[k] ? static_cast<F32>(-dir[k]) * 45.0f : 0.0f;  // driven to rail if no crossing
    }
}
// membranes at threshold -> target (units); estimate follows
void AmunNeuro::chargeTo(U8 mask, const F32 target[4]) {
    I32 x[4]; U32 us[4];
    for (U32 k = 0; k < 4; k++) {
        x[k] = 0; us[k] = 0;
        if (!(mask & (1U << k))) continue;
        const F32 delta = target[k] - m_est[k];           // zeroMask/readState leave m_est = where the chip is
        x[k] = (delta >= 0) ? 1 : -1;
        us[k] = static_cast<U32>(std::fabs(delta) * 1000.0f * (0.1f / m_w[k]));
    }
    F32 keep[4];
    for (U32 k = 0; k < 4; k++) keep[k] = m_est[k];
    this->pulseAll(x, us);
    for (U32 k = 0; k < 4; k++) m_est[k] = (mask & (1U << k)) ? target[k] : keep[k];
}
// rewrite the golden configuration of one neuron (weights + current input bytes); no reset
void AmunNeuro::scrubNeuron(U32 k) {
    U8 cf, cV2, cV1;
    caps(m_w[k], m_w[k], cf, cV2, cV1);
    const U8 d[3] = {cf, cV2, cV1};
    this->sendFrame(static_cast<U8>(k + 1), W_ADDR[k].bank, W_ADDR[k].byte, d, 3);
    this->sendFrame(static_cast<U8>(k + 1), V1_ADDR[k].bank, V1_ADDR[k].byte, &m_v1[k], 1);
    this->sendFrame(static_cast<U8>(k + 1), V2_ADDR[k].bank, V2_ADDR[k].byte, &m_v2[k], 1);
}
// arbitration: configuration is verifiable (F' holds the golden copy) -> always scrub it.
// state is not: trust the physical membrane unless it is saturated / implausible, then trust F'.
void AmunNeuro::repairNeuron(U32 k) {
    const F32 before = m_est[k];
    if (m_repair == Amun::RepairMode::NONE) return;
    if (m_repair == Amun::RepairMode::FULL_RESET && m_auto)
        this->log_WARNING_LO_BadArg(Fw::LogStringArg("FULL_RESET->scrub (autonomy)"));
    if (m_repair == Amun::RepairMode::FULL_RESET && !m_auto) {
        (void)this->primary();
        this->bootNeurons();
        for (U32 j = 0; j < 4; j++) m_mismatch[j] = 0;
        this->log_WARNING_HI_Repaired(static_cast<U8>(k + 1), Fw::LogStringArg("full reset"), before, m_est[k]);
        return;
    }
    this->scrubNeuron(k);
    F32 st[4], tgt[4] = {0, 0, 0, 0}; bool rail[4];
    this->readState(static_cast<U8>(1U << k), st, rail);
    const bool trustSw = rail[k] || std::fabs(st[k]) > EST_MAX + 6.0f;
    tgt[k] = clampf(trustSw ? before : st[k], m_floor[k], EST_MAX);
    this->chargeTo(static_cast<U8>(1U << k), tgt);
    this->log_WARNING_HI_Repaired(static_cast<U8>(k + 1), Fw::LogStringArg(trustSw ? "scrub+software" : "scrub+analog"),
                                  before, m_est[k]);
}
void AmunNeuro::checkDisagree() {
    for (U32 k = 0; k < 4; k++) {
        const bool above = this->positive(k);
        const bool pred = m_est[k] > 0.0f;
        if (std::fabs(m_est[k]) > DIS_MARGIN && above != pred) m_mismatch[k]++;
        else m_mismatch[k] = 0;
        if (m_mismatch[k] >= DIS_STEPS) {
            this->log_WARNING_HI_Disagree(static_cast<U8>(k + 1), m_est[k], above);
            m_mismatch[k] = 0;
            this->repairNeuron(k);
        }
    }
}
void AmunNeuro::writeTruth(U8 firedMask) {
    FILE* f = std::fopen(TRUTH_LOG, "a");
    if (f != nullptr) {
        std::fprintf(f, "%llu %.4f %.4f %.4f %.4f %u\n", static_cast<unsigned long long>(nowRtNs()),
                     static_cast<double>(m_est[0]), static_cast<double>(m_est[1]), static_cast<double>(m_est[2]),
                     static_cast<double>(m_est[3]), static_cast<unsigned>(firedMask));
        std::fclose(f);
    }
    FILE* h = std::fopen(HEARTBEAT, "w");
    if (h != nullptr) { std::fprintf(h, "%llu\n", static_cast<unsigned long long>(nowRtNs())); std::fclose(h); }
}

void AmunNeuro::CKPT_PERIOD_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U16 seconds) {
    m_ckPeriod = seconds; m_lastCkNs = 0;
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}
void AmunNeuro::REPAIR_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, const Amun::RepairMode& mode) {
    m_repair = mode;
    for (U32 k = 0; k < 4; k++) m_mismatch[k] = 0;
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}
void AmunNeuro::SCRUB_PERIOD_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U16 seconds) {
    m_scrubPeriod = seconds; m_lastScrubNs = 0;
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}
void AmunNeuro::UPSET_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U8 neuron, U8 kind) {
    if (!this->ready()) { this->log_WARNING_LO_NotReady(); this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR); return; }
    if (neuron < 1 || neuron > 4 || kind > 1) { this->log_WARNING_LO_BadArg(Fw::LogStringArg("neuron 1..4, kind 0..1")); this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::VALIDATION_ERROR); return; }
    const U32 k = neuron - 1U;
    U8 cf, cV2, cV1;
    caps(m_w[k], m_w[k], cf, cV2, cV1);
    U8 d[3] = {cf, cV2, cV1};
    const U32 idx = (kind == 0) ? 0U : 2U;
    const U8 before = d[idx];
    d[idx] = static_cast<U8>(d[idx] ^ ((kind == 0) ? 0x80 : 0x40));
    this->sendFrame(neuron, W_ADDR[k].bank, W_ADDR[k].byte, d, 3);     // software copy (m_w) NOT updated
    this->log_WARNING_LO_Upset(neuron, kind, before, d[idx]);
    this->cmdResponse_out(opCode, cmdSeq, m_errOk ? Fw::CmdResponse::OK : Fw::CmdResponse::EXECUTION_ERROR);
}
void AmunNeuro::CORRUPT_EST_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U8 neuron, F32 value) {
    if (neuron < 1 || neuron > 4) { this->log_WARNING_LO_BadArg(Fw::LogStringArg("neuron 1..4")); this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::VALIDATION_ERROR); return; }
    const F32 before = m_est[neuron - 1];
    m_est[neuron - 1] = value;
    this->log_WARNING_LO_CorruptedEst(neuron, before, value);
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}
void AmunNeuro::RECOVER_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, const Amun::RecoverSrc& src) {
    if (!this->ready()) { this->log_WARNING_LO_NotReady(); this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR); return; }
    m_monitor = false;
    const U64 now = nowRtNs();
    unsigned long long hb = 0;
    FILE* h = std::fopen(HEARTBEAT, "r");
    if (h != nullptr) { if (std::fscanf(h, "%llu", &hb) != 1) hb = 0; std::fclose(h); }
    const F32 ageS = (m_ckRtNs > 0) ? static_cast<F32>(now - m_ckRtNs) / 1e9f : -1.0f;
    const F32 downS = (hb > 0) ? static_cast<F32>(now - static_cast<U64>(hb)) / 1e9f : ageS;
    F32 st[4], tgt[4]; bool rail[4];
    this->readState(0x0F, st, rail);                 // every membrane read (all arms), left at threshold
    F32 corr[4];
    for (U32 k = 0; k < 4; k++) {
        corr[k] = st[k] + m_leak[k] * downS;   // undo the (weight-independent) drift since F' died
        if (src == Amun::RecoverSrc::ANALOG) tgt[k] = rail[k] ? m_ckEst[k] : corr[k];
        else if (src == Amun::RecoverSrc::CHECKPOINT) tgt[k] = m_ckEst[k];
        else tgt[k] = -BIAS;
        tgt[k] = clampf(tgt[k], EST_MIN, EST_MAX);
    }
    this->chargeTo(0x0F, tgt);
    for (U32 k = 0; k < 4; k++) {
        this->log_ACTIVITY_HI_Recovered(static_cast<U8>(k + 1), src, st[k], corr[k], m_ckEst[k], ageS, downS, tgt[k], rail[k]);
        m_fired[k] = false; m_mismatch[k] = 0;
    }
    m_gpsTrusted = true; m_lastStepNs = nowNs(); m_monitor = true;
    this->cmdResponse_out(opCode, cmdSeq, m_errOk ? Fw::CmdResponse::OK : Fw::CmdResponse::EXECUTION_ERROR);
}

// ======================================================================
// E10 / E11: chip-in-the-loop training and frozen-weight inference
// episode file lines: ep,mask,fix,sats,hdop,jump   (mask bit k = neuron k+1 should fire)
// ======================================================================
bool AmunNeuro::loadEpisodes(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (f == nullptr) { this->log_WARNING_HI_HwError(Fw::LogStringArg("open episode file"), errno); return false; }
    char line[128]; m_nSteps = 0;
    while (std::fgets(line, sizeof(line), f) != nullptr && m_nSteps < MAX_STEPS) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        unsigned ep = 0, mask = 0, fix = 0, sats = 0, jump = 0; double hdop = 0;
        if (std::sscanf(line, "%u,%u,%u,%u,%lf,%u", &ep, &mask, &fix, &sats, &hdop, &jump) != 6) continue;
        Step& s = m_steps[m_nSteps++];
        s.ep = static_cast<U16>(ep); s.mask = static_cast<U8>(mask); s.fix = static_cast<U8>(fix);
        s.sats = static_cast<U8>(sats); s.hdop = static_cast<F32>(hdop); s.jump = static_cast<U8>(jump);
    }
    std::fclose(f);
    if (m_nSteps == 0) { this->log_WARNING_LO_BadArg(Fw::LogStringArg("episode file empty")); return false; }
    return true;
}
void AmunNeuro::runEpisodes(bool learn, F32 eta, U16& falseAlarms, U16& misses, bool report, bool trained) {
    U16 faults[4] = {0, 0, 0, 0}, det[4] = {0, 0, 0, 0}, fa[4] = {0, 0, 0, 0}, nomEp[4] = {0, 0, 0, 0};
    F32 latSum[4] = {0, 0, 0, 0};
    U16 episodes = 0;
    falseAlarms = 0; misses = 0;
    U32 i = 0;
    while (i < m_nSteps) {
        const U16 ep = m_steps[i].ep;
        episodes++;
        // episode start: zero, bias (independent episodes)
        (void)this->zeroAll(60);               // leaves m_est = 0, or the measured state if a neuron failed
        { const F32 b[4] = {-BIAS, -BIAS, -BIAS, -BIAS}; this->chargeTo(0x0F, b); }
        I32 onset[4] = {-1, -1, -1, -1}, lastLab[4] = {-1, -1, -1, -1};
        bool detd[4] = {false, false, false, false}, faDone[4] = {false, false, false, false};
        bool anyLab[4] = {false, false, false, false}, prevFired[4] = {false, false, false, false};
        I32 s = 0;
        for (; i < m_nSteps && m_steps[i].ep == ep; i++, s++) {
            const U64 t0 = nowNs();
            const Step& st = m_steps[i];
            F32 e[4];
            e[0] = (st.fix == 0) ? 1.0f : -1.0f;
            e[1] = clampf((6.0f - static_cast<F32>(st.sats)) / 4.0f, -1.0f, 1.0f);
            e[2] = clampf((st.hdop - 2.0f) / 3.0f, -1.0f, 1.0f);
            e[3] = (st.jump != 0) ? 1.0f : -1.0f;
            for (U32 k = 0; k < 4; k++) m_est[k] = clampf(m_est[k] - m_leak[k] * (static_cast<F32>(STEP_MS) / 1000.0f), -45.0f, 45.0f);
            this->applyEvidence(e);
            for (U32 k = 0; k < 4; k++) {
                const bool fired = this->positive(k);
                const bool lab = (st.mask >> k) & 1U;
                if (lab) { anyLab[k] = true; lastLab[k] = s; if (onset[k] < 0) { onset[k] = s; faults[k]++; } }
                if (fired && onset[k] >= 0 && !detd[k] && s - onset[k] <= static_cast<I32>(DEADLINE) &&
                    (lab || s - lastLab[k] <= static_cast<I32>(GRACE))) {
                    detd[k] = true; det[k]++; latSum[k] += static_cast<F32>(s - onset[k]);
                }
                if (onset[k] >= 0 && !detd[k] && s - onset[k] == static_cast<I32>(DEADLINE)) {
                    misses++;
                    if (learn) {
                        const F32 w0 = m_w[k], w1 = clampf(w0 * (1.0f + eta), W_MIN, W_MAX);
                        this->setWeights(k, w1, w1);
                        this->log_ACTIVITY_LO_TrainUpdate(static_cast<U8>(k + 1), Fw::LogStringArg("late/miss"), w0, w1);
                    }
                }
                const bool quiet = !lab && (lastLab[k] < 0 || s - lastLab[k] > static_cast<I32>(GRACE));
                if (fired && !prevFired[k] && quiet && !faDone[k]) {     // new firing edge with no fault = false alarm
                    faDone[k] = true; fa[k]++; falseAlarms++;
                    if (learn) {
                        const F32 w0 = m_w[k], w1 = clampf(w0 * (1.0f - eta), W_MIN, W_MAX);
                        this->setWeights(k, w1, w1);
                        this->log_ACTIVITY_LO_TrainUpdate(static_cast<U8>(k + 1), Fw::LogStringArg("false alarm"), w0, w1);
                    }
                }
                prevFired[k] = fired;
            }
            const U64 el = nowNs() - t0;
            if (el < static_cast<U64>(STEP_MS) * 1000000ULL) sleepUs(static_cast<U32>((static_cast<U64>(STEP_MS) * 1000000ULL - el) / 1000ULL));
        }
        for (U32 k = 0; k < 4; k++) if (!anyLab[k]) nomEp[k]++;
    }
    if (report) {
        U16 tf = 0, td = 0; F32 tl = 0;
        for (U32 k = 0; k < 4; k++) {
            const F32 ml = (det[k] > 0) ? latSum[k] / static_cast<F32>(det[k]) : -1.0f;
            this->log_ACTIVITY_HI_InferNeuron(static_cast<U8>(k + 1), trained, faults[k], det[k], ml, fa[k], nomEp[k]);
            tf = static_cast<U16>(tf + faults[k]); td = static_cast<U16>(td + det[k]); tl += latSum[k];
        }
        this->log_ACTIVITY_HI_InferSummary(trained, tf, td, (td > 0) ? tl / static_cast<F32>(td) : -1.0f, falseAlarms, episodes);
    }
}
void AmunNeuro::TRAIN_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, const Fw::CmdStringArg& path, U8 epochs, F32 eta) {
    std::remove(HEARTBEAT);                // long blocking command: disarm the external watchdog
    if (!this->ready()) { this->log_WARNING_LO_NotReady(); this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR); return; }
    if (epochs < 1 || epochs > 20 || eta <= 0.0f || eta > 0.5f) { this->log_WARNING_LO_BadArg(Fw::LogStringArg("epochs 1..20, eta 0..0.5")); this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::VALIDATION_ERROR); return; }
    if (!this->loadEpisodes(path.toChar())) { this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR); return; }
    const bool wasMon = m_monitor; m_monitor = false;
    for (U8 ep = 1; ep <= epochs; ep++) {
        U16 fa = 0, mi = 0;
        this->runEpisodes(true, eta, fa, mi, false, true);
        this->log_ACTIVITY_HI_TrainEpoch(ep, fa, mi, m_w[0], m_w[1], m_w[2], m_w[3]);
    }
    FILE* f = std::fopen(WEIGHTS_FILE, "w");
    for (U32 k = 0; k < 4; k++) {
        m_wLearned[k] = m_w[k];
        if (f != nullptr) std::fprintf(f, "%.6f\n", static_cast<double>(m_w[k]));
    }
    if (f != nullptr) std::fclose(f);
    this->bootNeurons();                 // back to the monitoring baseline (mode weights)
    m_monitor = wasMon;
    this->cmdResponse_out(opCode, cmdSeq, m_errOk ? Fw::CmdResponse::OK : Fw::CmdResponse::EXECUTION_ERROR);
}
void AmunNeuro::INFER_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, const Fw::CmdStringArg& path, bool trained) {
    std::remove(HEARTBEAT);                // long blocking command: disarm the external watchdog
    if (!this->ready()) { this->log_WARNING_LO_NotReady(); this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR); return; }
    if (!this->loadEpisodes(path.toChar())) { this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR); return; }
    const bool wasMon = m_monitor; m_monitor = false;
    if (trained) {
        FILE* f = std::fopen(WEIGHTS_FILE, "r");      // learned weights survive a restart
        if (f != nullptr) {
            for (U32 k = 0; k < 4; k++) { double w = 0; if (std::fscanf(f, "%lf", &w) == 1) m_wLearned[k] = static_cast<F32>(w); }
            std::fclose(f);
        }
    }
    for (U32 k = 0; k < 4; k++) { const F32 w = trained ? m_wLearned[k] : 0.1f; this->setWeights(k, w, w); }
    U16 fa = 0, mi = 0;
    this->runEpisodes(false, 0.0f, fa, mi, true, trained);
    this->bootNeurons();
    m_monitor = wasMon;
    this->cmdResponse_out(opCode, cmdSeq, m_errOk ? Fw::CmdResponse::OK : Fw::CmdResponse::EXECUTION_ERROR);
}

// ======================================================================
// v7: autonomous, reset-surviving, self-learning fault protection
// (Alg. 0-6 of AMUN_v7_spec.md)
// ======================================================================
namespace {
bool fileExists(const char* p) { FILE* f = std::fopen(p, "r"); if (f) { std::fclose(f); return true; } return false; }
void busyMs(U32 ms) {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &b);
        const F64 el = static_cast<F64>(b.tv_sec - a.tv_sec) * 1e3 + static_cast<F64>(b.tv_nsec - a.tv_nsec) / 1e6;
        if (el >= static_cast<F64>(ms)) break;
    }
}
}  // namespace

const char* AmunNeuro::polName() const {
    static const char* const N[4] = {"AMUN", "B0", "B1", "B2"};
    return N[m_policy < 4 ? m_policy : 0];
}
// amun.cfg: key=value lines. policy=AMUN|B0|B1|B2  ckpt=N  learn=0|1  audit=A  clock=0|1  w0=  b0=  repair=NONE|SCRUB|FULL_RESET
void AmunNeuro::readConfig() {
    m_policy = P_AMUN; m_ckEvery = 0; m_learn = false; m_auditEvery = 0; m_clock = true; m_w0 = 0.1f; m_b0 = -18.0f;
    FILE* f = std::fopen(AMUN_CFG, "r");
    if (f == nullptr) return;
    char line[96];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        char key[32] = {0}, val[32] = {0};
        if (std::sscanf(line, " %31[^= ] = %31s", key, val) != 2) continue;
        if (!std::strcmp(key, "policy")) {
            if (!std::strcmp(val, "B0")) m_policy = P_B0;
            else if (!std::strcmp(val, "B1")) m_policy = P_B1;
            else if (!std::strcmp(val, "B2")) m_policy = P_B2;
            else m_policy = P_AMUN;
        } else if (!std::strcmp(key, "ckpt")) m_ckEvery = static_cast<U32>(std::atoi(val));
        else if (!std::strcmp(key, "learn")) m_learn = std::atoi(val) != 0;
        else if (!std::strcmp(key, "audit")) m_auditEvery = static_cast<U32>(std::atoi(val));
        else if (!std::strcmp(key, "clock")) m_clock = std::atoi(val) != 0;
        else if (!std::strcmp(key, "w0")) m_w0 = clampf(static_cast<F32>(std::atof(val)), W_MIN, W_MAX);
        else if (!std::strcmp(key, "b0")) m_b0 = static_cast<F32>(std::atof(val));
        else if (!std::strcmp(key, "repair")) {
            m_repair = !std::strcmp(val, "SCRUB") ? Amun::RepairMode::SCRUB
                     : (!std::strcmp(val, "FULL_RESET") ? Amun::RepairMode::FULL_RESET : Amun::RepairMode::NONE);
        }
    }
    std::fclose(f);
}
F32 AmunNeuro::rssKB() const {
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr) return 0;
    unsigned long size = 0, res = 0;
    const int n = std::fscanf(f, "%lu %lu", &size, &res);
    std::fclose(f);
    if (n != 2) return 0;
    return static_cast<F32>(res) * static_cast<F32>(sysconf(_SC_PAGESIZE)) / 1024.0f;
}
// Alg. 0b: CLOCK -> largest |drift| (best time resolution); then TIMING, MEMORY, GPS-LINK -> smallest |drift| first
void AmunNeuro::assignMonitors() {
    U32 idx[4] = {0, 1, 2, 3};
    for (U32 i = 0; i < 4; i++)
        for (U32 j = i + 1; j < 4; j++)
            if (std::fabs(m_leak[idx[j]]) < std::fabs(m_leak[idx[i]])) { const U32 t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
    m_nOf[M_TIMING] = static_cast<U8>(idx[0]); m_nOf[M_MEMORY] = static_cast<U8>(idx[1]);
    m_nOf[M_GPS] = static_cast<U8>(idx[2]); m_nOf[M_CLOCK] = static_cast<U8>(idx[3]);
    this->log_ACTIVITY_HI_Assigned(static_cast<U8>(idx[0] + 1), static_cast<U8>(idx[1] + 1), static_cast<U8>(idx[2] + 1),
                                   static_cast<U8>(idx[3] + 1));
}
void AmunNeuro::saveCal() {
    FILE* f = std::fopen(CAL_FILE, "w");
    if (f == nullptr) return;
    std::fprintf(f, "%s\n", "/home/amun/f/4n_H.ahf");
    for (U32 k = 0; k < 4; k++) std::fprintf(f, "%d %.6f\n", m_pos[k] ? 1 : 0, static_cast<double>(m_leak[k]));
    std::fprintf(f, "%u %u %u %u\n", m_nOf[0], m_nOf[1], m_nOf[2], m_nOf[3]);
    for (U32 i = 0; i < 3; i++) std::fprintf(f, "%.6f %.6f\n", static_cast<double>(m_off[i]), static_cast<double>(m_scale[i]));
    std::fclose(f);
}
bool AmunNeuro::loadCal() {
    FILE* f = std::fopen(CAL_FILE, "r");
    if (f == nullptr) return false;
    char path[128] = {0};
    bool ok = std::fscanf(f, "%127s", path) == 1 && this->loadImage(path);
    for (U32 k = 0; ok && k < 4; k++) {
        int p = 0; double lk = 0;
        ok = std::fscanf(f, "%d %lf", &p, &lk) == 2;
        m_pos[k] = p != 0; m_leak[k] = static_cast<F32>(lk);
    }
    unsigned a = 0, b = 1, c = 2, d = 3;
    ok = ok && std::fscanf(f, "%u %u %u %u", &a, &b, &c, &d) == 4;
    m_nOf[0] = static_cast<U8>(a & 3U); m_nOf[1] = static_cast<U8>(b & 3U); m_nOf[2] = static_cast<U8>(c & 3U); m_nOf[3] = static_cast<U8>(d & 3U);
    for (U32 i = 0; ok && i < 3; i++) {
        double o = 0, sc = 1;
        ok = std::fscanf(f, "%lf %lf", &o, &sc) == 2;
        m_off[i] = static_cast<F32>(o); m_scale[i] = static_cast<F32>(sc);
    }
    std::fclose(f);
    return ok;
}
void AmunNeuro::saveLearned() {        // NV write only when learning changes something
    FILE* f = std::fopen(LEARNED_FILE, "w");
    if (f == nullptr) return;
    for (U32 k = 0; k < 4; k++) std::fprintf(f, "%.6f %.6f\n", static_cast<double>(m_w[k]), static_cast<double>(m_floor[k]));
    std::fclose(f);
}
void AmunNeuro::loadLearned() {
    FILE* f = std::fopen(LEARNED_FILE, "r");
    if (f == nullptr) return;
    for (U32 k = 0; k < 4; k++) {
        double w = 0.1, b = EST_MIN;
        if (std::fscanf(f, "%lf %lf", &w, &b) != 2) break;
        m_w[k] = static_cast<F32>(w); m_floor[k] = static_cast<F32>(b);   // chip weights are already in its SRAM
    }
    std::fclose(f);
    for (U32 i = 0; i < 3; i++) m_wMon[i] = m_w[m_nOf[i]];
}
// non-essential workload; the system only ever sees its execution time and memory growth
void AmunNeuro::runTasks(F32& execMs) {
    const U64 t0 = nowNs();
    if (m_baseLeft > 0) { execMs = 0; return; }             // quiet baseline: non-essential work starts after it
    if (m_runaway && !m_shed[0]) { m_runN++; busyMs(m_runN * m_runDelta); }
    if (m_burstLeft > 0) { if (!m_shed[0]) busyMs(m_burstMs); m_burstLeft--; }
    const bool leakNow = m_leakOn && !m_shed[1];
    const bool cacheNow = m_cacheLeft > 0 && !m_shed[1];
    for (U32 pass = 0; pass < 2; pass++) {               // leak and cache never share a block
        const bool cache = (pass == 1);
        const U32 kb = cache ? (cacheNow ? m_cacheKB : 0U) : (leakNow ? m_leakKB : 0U);
        if (kb == 0 || m_nBlocks >= MAX_BLOCKS) continue;
        void* p = std::malloc(static_cast<size_t>(kb) * 1024U);
        if (p != nullptr) { std::memset(p, 0x5A, static_cast<size_t>(kb) * 1024U); m_blkCache[m_nBlocks] = cache; m_blocks[m_nBlocks++] = p; }
    }
    if (m_cacheLeft > 0 && --m_cacheLeft == 0) {          // benign cache: released by its owner when done
        U32 j = 0;
        for (U32 i = 0; i < m_nBlocks; i++) { if (m_blkCache[i]) std::free(m_blocks[i]); else { m_blkCache[j] = false; m_blocks[j++] = m_blocks[i]; } }
        m_nBlocks = j;
    }
    if (m_gpsBlockLeft > 0 && --m_gpsBlockLeft == 0) m_gpsBlock = false;
    execMs = static_cast<F32>(nowNs() - t0) / 1e6f;
}
void AmunNeuro::stopTasks(U32 mon) {    // isolation after a confirmed fault
    if (mon == M_TIMING) { m_runaway = false; m_burstLeft = 0; m_runN = 0; }
    if (mon == M_MEMORY) {
        m_leakOn = false; m_cacheLeft = 0;
        for (U32 i = 0; i < m_nBlocks; i++) std::free(m_blocks[i]);
        m_nBlocks = 0;
    }
    this->log_ACTIVITY_LO_TaskEvent(Fw::LogStringArg(mon == M_TIMING ? "isolated TIMING" : "isolated MEMORY"), 0);
}
// Alg. 4: consequence-labeled update of gain (10-byte frame) and floor (charge), evidence kept
void AmunNeuro::learnUpdate(U32 mon, bool up, const char* why) {
    if (!m_learn || mon > M_MEMORY) return;
    const U32 k = m_nOf[mon];
    const F32 w0 = m_w[k], b0 = m_floor[k];
    const F32 uMin = std::fmin(-std::fmin(40.0f, 45.0f - std::fabs(m_leak[k]) * D_REQ_S), FLOOR_MAX - 2.0f);
    const F32 w1 = clampf(w0 * (up ? 1.0f + ETA : 1.0f - ETA), W_MIN, W_MAX);
    const F32 b1 = clampf(b0 + (up ? BETA : -BETA), uMin, FLOOR_MAX);
    this->setWeights(k, w1, w1);
    m_floor[k] = b1; m_wMon[mon] = w1;
    this->saveLearned();
    this->log_ACTIVITY_HI_Learned(Fw::LogStringArg(MON_NAME[mon]), Fw::LogStringArg(why), w0, w1, b0, b1);
}
// Alg. 6b: round-robin magnitude audit of one monitor neuron (read, restore)
void AmunNeuro::audit() {
    const U32 mon = m_auditIdx++ % 3;
    const U32 k = m_nOf[mon];
    F32 st[4], tgt[4] = {0, 0, 0, 0}; bool rail[4];
    const F32 est = m_est[k];
    this->readState(static_cast<U8>(1U << k), st, rail);
    tgt[k] = clampf(rail[k] ? est : st[k], m_floor[k], EST_MAX);
    this->chargeTo(static_cast<U8>(1U << k), tgt);
    if (std::fabs(st[k] - est) > DIS_MARGIN) this->log_WARNING_LO_AuditFix(static_cast<U8>(k + 1), est, st[k]);
}

void AmunNeuro::autoStep() {
    const U64 tNow = nowNs();
    F32 execMs = 0;
    this->runTasks(execMs);
    const F32 rss = this->rssKB();
    const F32 grow = (m_rssPrev > 0) ? rss - m_rssPrev : 0.0f;
    m_rssPrev = rss;
    if (m_rssBase > 0 && rss - m_rssBase > MEM_LIMIT_KB) {          // consequence: memory exhausted
        this->log_WARNING_HI_OomKill(static_cast<U32>(rss));
        ::raise(SIGKILL);
    }
    const F32 age = (m_lastNmeaNs > 0) ? static_cast<F32>(nowNs() - m_lastNmeaNs) / 1e9f : 0.0f;
    const F32 f[3] = {execMs, grow, age};
    F32 dt = (m_lastStepNs == 0) ? 1.0f : static_cast<F32>(tNow - m_lastStepNs) / 1e9f;
    if (dt > 5.0f) dt = 5.0f;
    m_lastStepNs = tNow;
    for (U32 k = 0; k < 4; k++) m_est[k] = clampf(m_est[k] - m_leak[k] * dt, -45.0f, 45.0f);
    F32 e[4] = {0, 0, 0, 0}, xs[3] = {-1, -1, -1};
    if (m_baseLeft > 0) {                                    // Alg. 0: quiet baseline -> offsets (mean + 3 sigma)
        for (U32 i = 0; i < 3; i++) { m_bSum[i] += f[i]; m_bSq[i] += static_cast<F64>(f[i]) * f[i]; e[m_nOf[i]] = -1.0f; }
        m_bN++;
        if (--m_baseLeft == 0) {
            const F32 scale[3] = {WATCHDOG_S * 1000.0f / 2.0f, MEM_LIMIT_KB / 10.0f, GPS_FAIL_S};  // mission-derived
            for (U32 i = 0; i < 3; i++) {
                const F64 mu = m_bSum[i] / m_bN, var = std::fmax(0.0, m_bSq[i] / m_bN - mu * mu);
                m_off[i] = static_cast<F32>(mu + 3.0 * std::sqrt(var)); m_scale[i] = scale[i];
                this->log_ACTIVITY_HI_Baseline(Fw::LogStringArg(MON_NAME[i]), m_off[i], m_scale[i]);
            }
            this->saveCal();
        }
    } else {
        for (U32 i = 0; i < 3; i++) {
            const F32 xr = (f[i] - m_off[i]) / m_scale[i];
            xs[i] = (xr < 0.05f) ? -1.0f : clampf(xr, -1.0f, 1.0f);   // healthy band -> full healthy evidence
            e[m_nOf[i]] = xs[i];
        }
    }
    this->applyEvidence(e);
    if (m_clock) {                                           // Alg. 5: clock neuron back to V0 every tick
        const U32 c = m_nOf[M_CLOCK];
        (void)this->zeroMask(static_cast<U8>(1U << c), 60);
        F32 t[4] = {0, 0, 0, 0}; t[c] = V0_CLOCK;
        this->chargeTo(static_cast<U8>(1U << c), t);
    }
    // Alg. 3: fault-protection FSM
    U8 mask = 0;
    for (U32 mon = 0; mon < 3; mon++) {
        const U32 k = m_nOf[mon];
        const bool y = (m_baseLeft == 0) && this->positive(k);
        if (y) mask |= static_cast<U8>(1U << k);
        if (m_probing[mon]) {
            if (xs[mon] > PROBE_HIT) m_probeHit[mon] = true;
            if (--m_probeLeft[mon] == 0 || m_probeHit[mon]) {
                m_probing[mon] = false; m_fsm[mon] = S_NOM;
                this->log_ACTIVITY_HI_Probe(Fw::LogStringArg(MON_NAME[mon]), m_probeHit[mon]);
                if (m_probeHit[mon]) this->stopTasks(mon);
                else this->learnUpdate(mon, false, "false shed");
            }
            continue;
        }
        switch (m_fsm[mon]) {
            case S_NOM:
                if (y) { m_fsm[mon] = S_WARN; m_fsmCnt[mon] = 1; this->log_WARNING_LO_Warn(Fw::LogStringArg(MON_NAME[mon]), xs[mon], m_est[k]); }
                break;
            case S_WARN:
                if (!y) { m_fsm[mon] = S_NOM; break; }
                if (++m_fsmCnt[mon] >= P_WARN && mon != M_GPS) {
                    m_fsm[mon] = S_SHED; m_shed[mon] = true; m_shedNs[mon] = tNow;
                    if (mon == M_MEMORY) { for (U32 i = 0; i < m_nBlocks; i++) std::free(m_blocks[i]); m_nBlocks = 0; }
                    this->log_WARNING_HI_Shed(Fw::LogStringArg(MON_NAME[mon]), execMs, grow);
                }
                break;
            default:   // S_SHED: after H_PROBE resume the work and watch whether the misbehaviour returns
                if (static_cast<F32>(tNow - m_shedNs[mon]) / 1e9f >= H_PROBE_S) {
                    m_shed[mon] = false; m_runN = (mon == M_TIMING) ? 0 : m_runN;
                    m_probing[mon] = true; m_probeLeft[mon] = P_PROBE; m_probeHit[mon] = false;
                }
                break;
        }
    }
    this->checkDisagree();
    if (m_auditEvery > 0 && (m_lastAuditNs == 0 || tNow - m_lastAuditNs >= static_cast<U64>(m_auditEvery) * 1000000000ULL)) {
        this->audit(); m_lastAuditNs = tNow;
    }
    if (m_policy == P_B2 && m_ckEvery > 0 &&
        (m_lastPolCkNs == 0 || tNow - m_lastPolCkNs >= static_cast<U64>(m_ckEvery) * 1000000000ULL)) {
        this->saveCheckpoint(); m_lastPolCkNs = tNow;
    }
    this->tlmWrite_FiredMask(mask);
    this->writeTruth(mask);                                  // evaluation log + liveness heartbeat
}

// load persisted configuration items (the classic reset-loop carrier), honouring SAFE
static U8 loadFaults(AmunNeuro* self, bool safe, const char* cause, bool& runaway, U32& runDelta, bool& leak, U32& leakKB) {
    (void)self;
    FILE* f = std::fopen(FAULT_CFG, "r");
    if (f == nullptr) return 0;
    char kind[16]; unsigned v = 0; U8 n = 0;
    while (std::fscanf(f, "%15s %u", kind, &v) == 2) {
        const bool timing = !std::strcmp(kind, "RUNAWAY"), memory = !std::strcmp(kind, "LEAK");
        if (safe && (!std::strcmp(cause, "ALL") || (timing && !std::strcmp(cause, "TIMING")) || (memory && !std::strcmp(cause, "MEMORY"))))
            continue;                                         // SAFE: boot that subsystem with defaults
        if (timing) { runaway = true; runDelta = v; n++; }
        if (memory) { leak = true; leakKB = v; n++; }
    }
    std::fclose(f);
    return n;
}

// Alg. 2: warm start after any death (no reset, no reconfiguration)
bool AmunNeuro::warmStart() {
    if (!fileExists(WARM_FLAG)) return false;
    this->readConfig();
    if (!this->loadCal() || !this->hwOpen(0, true)) return false;
    m_errOk = true; m_frames = 0; m_calibrated = true;
    this->loadLearned();
    for (U32 k = 0; k < 4; k++) {               // F' may have died mid-pulse: force HOLD (identical bytes otherwise)
        const U8 p = PLUS, m = MINUS;
        this->sendFrame(static_cast<U8>(k + 1), V1_ADDR[k].bank, V1_ADDR[k].byte, &p, 1);
        this->sendFrame(static_cast<U8>(k + 1), V2_ADDR[k].bank, V2_ADDR[k].byte, &m, 1);
        m_v1[k] = PLUS; m_v2[k] = MINUS; m_fired[k] = false; m_mismatch[k] = 0;
    }
    m_booted = true;
    const U64 now = nowRtNs();
    unsigned long long hb = 0;
    FILE* h = std::fopen(HEARTBEAT, "r");
    if (h != nullptr) { if (std::fscanf(h, "%llu", &hb) != 1) hb = 0; std::fclose(h); }
    const F32 downS = (hb > 0 && now > static_cast<U64>(hb)) ? static_cast<F32>(now - static_cast<U64>(hb)) / 1e9f : -1.0f;
    F32 st[4], tgt[4]; bool rail[4];
    this->readState(0x0F, st, rail);
    const U32 c = m_nOf[M_CLOCK];
    const F32 clockS = (m_clock && !rail[c] && std::fabs(m_leak[c]) > 0.1f) ? (V0_CLOCK - st[c]) / m_leak[c] : -1.0f;
    const F32 dCorr = (downS > 0) ? downS : (clockS > 0 ? clockS : 0.0f);
    if (m_policy == P_B2) {                                   // B2: only the estimate from the last software checkpoint
        FILE* cf = std::fopen(CHECKPOINT, "r");
        for (U32 k = 0; k < 4; k++) m_ckEst[k] = m_floor[k];
        if (cf != nullptr) {
            for (U32 k = 0; k < 4; k++) {
                int p = 0; unsigned int cu = 0; double lk = 0, w = 0.1, es = 0;
                if (std::fscanf(cf, "%d %u %lf %lf %lf", &p, &cu, &lk, &w, &es) != 5) break;
                m_ckEst[k] = static_cast<F32>(es);
            }
            std::fclose(cf);
        }
    }
    for (U32 k = 0; k < 4; k++) tgt[k] = m_floor[k];
    for (U32 mon = 0; mon < 3; mon++) {
        const U32 k = m_nOf[mon];
        F32 u = m_floor[k];
        if (m_policy == P_AMUN) u = rail[k] ? m_floor[k] : st[k] + m_leak[k] * dCorr;
        else if (m_policy == P_B2) u = m_ckEst[k];
        tgt[k] = clampf(u, m_floor[k], EST_MAX);
    }
    tgt[c] = V0_CLOCK;
    {   // diagnostics: raw membrane read, drift-corrected value and what is applied, every neuron
        const Amun::RecoverSrc src = (m_policy == P_AMUN) ? Amun::RecoverSrc::ANALOG
                                   : (m_policy == P_B2 ? Amun::RecoverSrc::CHECKPOINT : Amun::RecoverSrc::COLD);
        for (U32 k = 0; k < 4; k++)
            this->log_ACTIVITY_HI_Recovered(static_cast<U8>(k + 1), src, st[k], st[k] + m_leak[k] * dCorr,
                                            m_ckEst[k], m_floor[k], downS, tgt[k], rail[k]);
    }
    // attribution: how far each monitor had travelled from its floor toward threshold when F' died
    I32 cause = -1; F32 best = 0;
    for (U32 mon = 0; mon < 2; mon++) {
        const U32 k = m_nOf[mon];
        const F32 prog = (tgt[k] - m_floor[k]) / (-m_floor[k]);
        if (prog >= ATTRIB && prog > best) { best = prog; cause = static_cast<I32>(mon); }
    }
    // the death is the consequence that closes the attributed monitor's evidence episode
    if (cause >= 0) tgt[m_nOf[cause]] = m_floor[m_nOf[cause]];
    this->chargeTo(0x0F, tgt);
    // death record (also the B1 boot counter)
    U32 deaths = 1;
    FILE* bl = std::fopen(BOOTS_LOG, "r");
    if (bl != nullptr) {
        unsigned long long t = 0; char cs[16];
        while (std::fscanf(bl, "%llu %15s", &t, cs) == 2)
            if (static_cast<F32>(now - static_cast<U64>(t)) / 1e9f <= B1_T_S) deaths++;
        std::fclose(bl);
    }
    bl = std::fopen(BOOTS_LOG, "a");
    if (bl != nullptr) { std::fprintf(bl, "%llu %s\n", static_cast<unsigned long long>(now), cause >= 0 ? MON_NAME[cause] : "unknown"); std::fclose(bl); }
    this->log_WARNING_HI_Death(downS, clockS, Fw::LogStringArg(cause >= 0 ? MON_NAME[cause] : "unknown"), best, Fw::LogStringArg(this->polName()));
    if (cause >= 0) this->learnUpdate(static_cast<U32>(cause), true, "death");   // learning from its own death
    // escalation (loop breaking)
    m_safe = fileExists(SAFE_FLAG);
    if (m_safe) { FILE* sf = std::fopen(SAFE_FLAG, "r"); if (sf) { if (std::fscanf(sf, "%15s", m_safeCause) != 1) std::strcpy(m_safeCause, "ALL"); std::fclose(sf); } }
    bool esc = false; const char* escCause = "ALL";
    if ((m_policy == P_AMUN || m_policy == P_B2) && cause >= 0) { esc = true; escCause = MON_NAME[cause]; }
    if (m_policy == P_B1 && deaths >= B1_K) { esc = true; escCause = "ALL"; }
    if (esc && !m_safe) {
        m_safe = true; std::strncpy(m_safeCause, escCause, sizeof(m_safeCause) - 1);
        FILE* sf = std::fopen(SAFE_FLAG, "w"); if (sf) { std::fprintf(sf, "%s\n", m_safeCause); std::fclose(sf); }
        this->log_WARNING_HI_Escalated(Fw::LogStringArg(m_safeCause), Fw::LogStringArg(this->polName()), deaths);
    }
    m_runaway = false; m_leakOn = false; m_runN = 0;
    const U8 nf = loadFaults(this, m_safe, m_safeCause, m_runaway, m_runDelta, m_leakOn, m_leakKB);
    this->log_ACTIVITY_HI_WarmStart(Fw::LogStringArg(this->polName()), m_safe, nf);
    for (U32 i = 0; i < 3; i++) { m_fsm[i] = S_NOM; m_probing[i] = false; }
    m_shed[0] = m_shed[1] = false;
    m_baseLeft = 0; m_rssPrev = 0; m_rssBase = this->rssKB(); m_lastStepNs = nowNs(); m_lastAuditNs = 0; m_lastPolCkNs = 0;
    m_auto = true;
    return true;
}

void AmunNeuro::AUTONOMY_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, bool enable) {
    if (!enable) {
        m_auto = false; std::remove(WARM_FLAG); std::remove(HEARTBEAT);
        this->stopTasks(M_TIMING); this->stopTasks(M_MEMORY);
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK); return;
    }
    if (!this->ready()) { this->log_WARNING_LO_NotReady(); this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR); return; }
    m_monitor = false;
    this->readConfig();
    std::remove(SAFE_FLAG); std::remove(BOOTS_LOG); std::remove(LEARNED_FILE);   // fresh experiment
    m_safe = false; m_safeCause[0] = '\0';
    this->assignMonitors();
    for (U32 mon = 0; mon < 3; mon++) {
        const U32 k = m_nOf[mon];
        const F32 w = (mon == M_GPS) ? 0.1f : m_w0;
        const F32 uMin = std::fmin(-std::fmin(40.0f, 45.0f - std::fabs(m_leak[k]) * D_REQ_S), FLOOR_MAX - 2.0f);
        this->setWeights(k, w, w); m_wMon[mon] = w;
        m_floor[k] = clampf(m_b0, uMin, FLOOR_MAX);
    }
    (void)this->zeroAll(60);
    { F32 t[4]; for (U32 k = 0; k < 4; k++) t[k] = m_floor[k]; t[m_nOf[M_CLOCK]] = V0_CLOCK; this->chargeTo(0x0F, t); }
    this->saveLearned();
    m_baseLeft = BASE_TICKS; m_bN = 0;
    for (U32 i = 0; i < 3; i++) { m_bSum[i] = 0; m_bSq[i] = 0; m_fsm[i] = S_NOM; m_probing[i] = false; }
    m_shed[0] = m_shed[1] = false;
    m_runaway = false; m_leakOn = false; m_runN = 0;
    const U8 nf = loadFaults(this, false, "", m_runaway, m_runDelta, m_leakOn, m_leakKB);
    m_rssPrev = 0; m_rssBase = this->rssKB(); m_lastStepNs = nowNs(); m_lastAuditNs = 0; m_lastPolCkNs = 0;
    this->saveCal();
    FILE* f = std::fopen(WARM_FLAG, "w"); if (f) { std::fprintf(f, "auto\n"); std::fclose(f); }
    this->log_ACTIVITY_HI_WarmStart(Fw::LogStringArg(this->polName()), false, nf);
    m_auto = true;
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void AmunNeuro::TASK_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, const Amun::TaskKind& kind, U32 param) {
    const char* what = "?";
    switch (kind.e) {
        case Amun::TaskKind::RUNAWAY: m_runaway = true; m_runDelta = param; m_runN = 0; what = "RUNAWAY"; break;
        case Amun::TaskKind::BURST: m_burstLeft = 3; m_burstMs = param; what = "BURST"; break;
        case Amun::TaskKind::LEAK: m_leakOn = true; m_leakKB = param; what = "LEAK"; break;
        case Amun::TaskKind::CACHE: m_cacheLeft = 4; m_cacheKB = param; what = "CACHE"; break;
        case Amun::TaskKind::GPS_BLOCK: m_gpsBlock = true; m_gpsBlockLeft = param; what = "GPS_BLOCK"; break;
        default: this->stopTasks(M_TIMING); this->stopTasks(M_MEMORY); m_gpsBlock = false; what = "CLEAR"; break;
    }
    this->log_ACTIVITY_LO_TaskEvent(Fw::LogStringArg(what), param);
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

}  // namespace Amun
