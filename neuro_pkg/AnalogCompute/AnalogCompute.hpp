// ======================================================================
// \title  AnalogCompute.hpp
// \brief  PIKA HAT (4x AN231E04) manager for F Prime: primary configuration,
//         in-place partial reconfiguration, comparator readout.
//
// Hardware access mirrors fpaa-tools 0.4.0 exactly (same pins, CE0 held low
// by GPIO for the session, SPI_NO_CS, 64-byte chunks, 0x00 dummy after each
// update frame) so F' and the Python lab scripts drive the chip identically.
// ======================================================================

#ifndef Amun_AnalogCompute_HPP
#define Amun_AnalogCompute_HPP

#include "Amun/Components/AnalogCompute/AnalogComputeComponentAc.hpp"

namespace Amun {

class AnalogCompute final : public AnalogComputeComponentBase {
  public:
    AnalogCompute(const char* const compName);
    ~AnalogCompute();

  private:
    // ------------------------------------------------------------------
    // Port / command handlers
    // ------------------------------------------------------------------
    void run_handler(FwIndexType portNum, U32 context) override;

    void OPEN_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U32 spiHz) override;
    void PRIMARY_CONFIG_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, const Fw::CmdStringArg& path) override;
    void LOAD_FRAME_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U8 slot, const Fw::CmdStringArg& path) override;
    void SEND_FRAME_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U8 slot) override;
    void SEND_AND_TIME_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U8 slot, U8 level, U32 timeoutMs) override;
    void CLOSE_cmdHandler(FwOpcodeType opCode, U32 cmdSeq) override;

    // ------------------------------------------------------------------
    // Hardware helpers
    // ------------------------------------------------------------------
    bool hwOpen(U32 spiHz);
    void hwClose();
    bool isOpen() const { return m_gpio != nullptr && m_spiFd >= 0; }
    void pinMode(U32 pin, bool output);
    void pinWrite(U32 pin, bool high);
    bool pinRead(U32 pin) const;
    bool spiWrite(const U8* data, U32 len);
    bool sendSlot(U8 slot, U32& latencyUs);
    static U64 nowNs();
    void housekeeping();

    static constexpr U32 NUM_SLOTS = 16;
    static constexpr U32 MAX_FRAME = 1024;
    static constexpr U32 MAX_PRIMARY = 8192;

    U8 m_frames[NUM_SLOTS][MAX_FRAME];
    U32 m_frameLen[NUM_SLOTS];
    U8 m_primary[MAX_PRIMARY];

    volatile U32* m_gpio;  // /dev/gpiomem mapping (BCM2711 GPIO block)
    int m_gpioFd;
    int m_spiFd;
    U32 m_spiHz;

    U32 m_updates;
    U32 m_updateErrors;
    U32 m_timeouts;
};

}  // namespace Amun

#endif
