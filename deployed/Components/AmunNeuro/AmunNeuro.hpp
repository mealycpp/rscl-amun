// ======================================================================
// AmunNeuro.hpp - four analog evidence neurons (one per AN231E04) driven
// through the configuration interface, plus a GPS integrity monitor.
// Hardware access mirrors fpaa-tools 0.4.0 (CE0 held low, SPI_NO_CS,
// 64-byte chunks, 0x00 dummy after each update frame).
// ======================================================================
#ifndef Amun_AmunNeuro_HPP
#define Amun_AmunNeuro_HPP

#include "Amun/Components/AmunNeuro/AmunNeuroComponentAc.hpp"

namespace Amun {

class AmunNeuro final : public AmunNeuroComponentBase {
  public:
    explicit AmunNeuro(const char* const compName);
    ~AmunNeuro();

  private:
    // handlers
    void run_handler(FwIndexType portNum, U32 context) override;
    void BOOT_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, const Fw::CmdStringArg& path, U32 spiHz) override;
    void SET_INPUT_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U8 neuron, I8 x) override;
    void PULSE_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U8 neuron, I8 x, U32 us) override;
    void SET_WEIGHTS_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U8 neuron, F32 wV1, F32 wV2) override;
    void CARRY_TRIAL_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U16 tMs, U32 holdMs, F32 wNew, bool restore) override;
    void MONITOR_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, bool enable) override;
    void MODE_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, const Amun::NeuroMode& mode) override;
    void RESET_ON_MODE_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, bool enable) override;
    void INJECT_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U8 neuron, I8 pct, U16 seconds) override;
    void CLOSE_cmdHandler(FwOpcodeType opCode, U32 cmdSeq) override;
    void CHARGE_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U16 tMs) override;
    void READOUT_cmdHandler(FwOpcodeType opCode, U32 cmdSeq) override;
    void CRASH_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, bool inRun) override;
    void REATTACH_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, const Fw::CmdStringArg& path, bool writeHold) override;
    void CKPT_PERIOD_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U16 seconds) override;
    void RECOVER_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, const Amun::RecoverSrc& src) override;
    void REPAIR_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, const Amun::RepairMode& mode) override;
    void SCRUB_PERIOD_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U16 seconds) override;
    void UPSET_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U8 neuron, U8 kind) override;
    void CORRUPT_EST_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U8 neuron, F32 value) override;
    void TRAIN_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, const Fw::CmdStringArg& path, U8 epochs, F32 eta) override;
    void INFER_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, const Fw::CmdStringArg& path, bool trained) override;
    void AUTONOMY_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, bool enable) override;
    void TASK_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, const Amun::TaskKind& kind, U32 param) override;
    bool loadImage(const char* path);
    void saveCheckpoint();
    bool loadCheckpoint();

    // hardware
    bool hwOpen(U32 spiHz, bool keepRunning = false);
    void hwClose();
    bool ready() const { return m_gpio != nullptr && m_spiFd >= 0 && m_booted; }
    void pinMode(U32 pin, bool output);
    void pinWrite(U32 pin, bool high);
    bool pinRead(U32 pin) const;
    bool spiWrite(const U8* data, U32 len);
    bool sendFrame(U8 chip, U8 bank, U8 byte, const U8* data, U8 n);
    bool primary();
    static U64 nowNs();
    static U64 nowRtNs();          // wall clock: survives a process restart

    // neuron primitives (k = 0..3 -> chip k+1)
    bool comp(U32 k) const;           // raw comparator level
    bool positive(U32 k) const;       // membrane above threshold
    void setInput(U32 k, I32 x);      // +1 / 0 / -1 by rule
    void setWeights(U32 k, F32 wV1, F32 wV2);
    void calibrate();
    bool zeroAll(U32 timeoutMs);
    void bootNeurons();
    void pulseAll(const I32 x[4], const U32 us[4]);

    // GPS
    void gpsPoll();
    void gpsLine(const char* s);
    void monitorStep();

    static constexpr U32 MAX_PRIMARY = 8192;
    U8 m_primary[MAX_PRIMARY];
    U32 m_primaryLen;

    volatile U32* m_gpio;
    int m_gpioFd;
    int m_spiFd;
    U32 m_spiHz;
    bool m_booted;
    bool m_errOk;
    U32 m_frames;

    // per-neuron state
    U8 m_v1[4];                  // current V1 byte (0x51 / 0x41)
    U8 m_v2[4];
    bool m_pos[4];               // comparator level meaning "membrane positive"
    F32 m_w[4];                  // current weight (V1 == V2 in normal operation)
    F32 m_est[4];                // rail guard: bounded estimate, ms of +1 at w=0.1
    bool m_fired[4];
    I8 m_injPct[4];
    U32 m_injLeft[4];

    // monitor
    bool m_monitor;
    bool m_resetOnMode;
    Amun::NeuroMode m_mode;
    bool m_gpsTrusted;

    // GPS parse state
    int m_ttyFd;
    char m_line[128];
    U32 m_lineLen;
    U8 m_fix;
    U8 m_sats;
    F32 m_hdop;
    bool m_haveFix;
    F64 m_lat, m_lon, m_prevLat, m_prevLon;
    bool m_havePrev;
    F32 m_speedMps;
    bool m_jump;
    U32 m_chargedUs[4];          // last known charge per neuron (checkpoint)
    bool m_calibrated;
    F32 m_leak[4];               // hold drift per neuron, units/s at w=0.1 (+ = state decays downward)
    U64 m_lastStepNs;
    U64 m_chargeRtNs;            // wall-clock end of the last CHARGE (checkpoint)

    // E8b / E9 / E10-11
    bool zeroMask(U8 mask, U32 timeoutMs);
    void readState(U8 mask, F32 st[4], bool rail[4]);   // destructive: leaves membranes at threshold
    void chargeTo(U8 mask, const F32 target[4]);        // from threshold to target (units), sets m_est
    void applyEvidence(const F32 e[4]);                 // ternary pulses with rail guard + floor
    void scrubNeuron(U32 k);
    void repairNeuron(U32 k);
    void checkDisagree();
    void writeTruth(U8 firedMask);
    bool loadEpisodes(const char* path);
    void runEpisodes(bool learn, F32 eta, U16& falseAlarms, U16& misses, bool report, bool trained);
    F32 m_ckEst[4];              // software estimate in the checkpoint
    U64 m_ckRtNs;                // wall clock of that checkpoint
    U16 m_ckPeriod;
    U64 m_lastCkNs;
    U16 m_scrubPeriod;
    U64 m_lastScrubNs;
    U32 m_scrubs;
    Amun::RepairMode m_repair;
    U8 m_mismatch[4];
    F32 m_wLearned[4];
    struct Step { U16 ep; U8 mask; U8 fix; U8 sats; U8 jump; F32 hdop; };
    static constexpr U32 MAX_STEPS = 6000;
    Step m_steps[MAX_STEPS];
    U32 m_nSteps;

    // ---------------- v7 autonomy ----------------
    enum Mon { M_TIMING = 0, M_MEMORY = 1, M_GPS = 2, M_CLOCK = 3, N_MON = 4 };
    enum Fsm { S_NOM = 0, S_WARN = 1, S_SHED = 2 };
    enum Pol { P_AMUN = 0, P_B0 = 1, P_B1 = 2, P_B2 = 3 };
    void autoStep();
    bool warmStart();
    void readConfig();
    void assignMonitors();
    void saveCal();
    bool loadCal();
    void saveLearned();
    void loadLearned();
    void runTasks(F32& execMs);
    void stopTasks(U32 mon);
    void learnUpdate(U32 mon, bool up, const char* why);
    void audit();
    F32 rssKB() const;
    const char* polName() const;
    bool m_auto;
    bool m_warmChecked;
    U8 m_nOf[N_MON];             // neuron index for each monitor
    F32 m_off[3], m_scale[3];    // feature offset / scale (TIMING, MEMORY, GPS)
    F32 m_floor[4];              // per-neuron floor (learned bias), units
    F32 m_wMon[3];               // per-monitor gain
    U32 m_baseLeft; F64 m_bSum[3], m_bSq[3]; U32 m_bN;
    U32 m_fsm[3], m_fsmCnt[3]; U64 m_shedNs[3]; bool m_probing[3]; U32 m_probeLeft[3]; bool m_probeHit[3];
    bool m_shed[2];              // non-essential work of TIMING / MEMORY classes stopped
    // workload
    bool m_runaway; U32 m_runDelta, m_runN; U32 m_burstLeft, m_burstMs;
    bool m_leakOn; U32 m_leakKB; U32 m_cacheLeft, m_cacheKB; bool m_gpsBlock; U32 m_gpsBlockLeft = 0;
    static constexpr U32 MAX_BLOCKS = 4096;
    void* m_blocks[MAX_BLOCKS]; U32 m_nBlocks; bool m_blkCache[MAX_BLOCKS];
    F32 m_rssPrev, m_rssBase;
    U64 m_prevTickNs; U64 m_lastNmeaNs;
    // configuration (amun.cfg)
    U32 m_policy; U32 m_ckEvery; bool m_learn; U32 m_auditEvery; bool m_clock; F32 m_w0, m_b0;
    U64 m_lastAuditNs; U32 m_auditIdx; U64 m_lastPolCkNs;
    bool m_safe; char m_safeCause[16];
    void measureLeak();           // polarity learned once at BOOT (fixed hardware property)
};

}  // namespace Amun
#endif
