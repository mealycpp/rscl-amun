// ======================================================================
// \title  AnalogCompute.cpp
// \brief  PIKA HAT (4x AN231E04) manager for F Prime
// ======================================================================

#include "Amun/Components/AnalogCompute/AnalogCompute.hpp"

#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace Amun {

// ----------------------------------------------------------------------
// PIKA pin map (Okika Pi.Ka quick-start, header table) -- BCM numbering
// ----------------------------------------------------------------------
namespace {
constexpr U32 PIN_CE0 = 8;           // chip select, held low by GPIO all session
constexpr U32 PIN_RESET_B = 26;      // active-low chain reset
constexpr U32 PIN_CFGFLG_B = 23;     // open drain, input
constexpr U32 PIN_ACTIVATE = 24;     // open drain, input (high = all chips configured)
constexpr U32 PIN_ERR_B = 25;        // open drain, input (high = no error)
constexpr U32 PIN_LCL_ACLK_EN = 14;  // enable on-board 16 MHz oscillator
constexpr U32 PIN_ACLK_SEL0 = 5;
constexpr U32 PIN_ACLK_SEL1 = 6;
constexpr U32 PIN_COMP = 16;  // FPAA0 IO5P = on-chip comparator output

// BCM2711 GPIO register word offsets inside /dev/gpiomem
constexpr U32 GPFSEL0 = 0x00 / 4;
constexpr U32 GPSET0 = 0x1C / 4;
constexpr U32 GPCLR0 = 0x28 / 4;
constexpr U32 GPLEV0 = 0x34 / 4;

constexpr U32 DEFAULT_SPI_HZ = 32000000;  // fpaa-tools default
constexpr U32 SPI_CHUNK = 64;             // fpaa-tools chunking
constexpr U32 RESET_LOW_US = 20000;       // >= 20 ms RESET_B low
constexpr U32 RESET_SETTLE_US = 100000;   // 100 ms before SPI
constexpr U32 AUTONULL_US = 60000;        // auto-null after primary config

void sleepUs(U32 us) {
    struct timespec ts;
    ts.tv_sec = us / 1000000;
    ts.tv_nsec = static_cast<long>(us % 1000000) * 1000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

// Read a whole file into buf; returns bytes read or -1
I32 readFile(const char* path, U8* buf, U32 cap) {
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    U32 total = 0;
    while (total < cap) {
        ssize_t n = ::read(fd, buf + total, cap - total);
        if (n < 0) {
            ::close(fd);
            return -1;
        }
        if (n == 0) {
            break;
        }
        total += static_cast<U32>(n);
    }
    U8 extra;
    bool overflow = (total == cap) && (::read(fd, &extra, 1) == 1);
    ::close(fd);
    return overflow ? -1 : static_cast<I32>(total);
}
}  // namespace

// ----------------------------------------------------------------------
// Construction
// ----------------------------------------------------------------------

AnalogCompute ::AnalogCompute(const char* const compName)
    : AnalogComputeComponentBase(compName),
      m_gpio(nullptr),
      m_gpioFd(-1),
      m_spiFd(-1),
      m_spiHz(DEFAULT_SPI_HZ),
      m_updates(0),
      m_updateErrors(0),
      m_timeouts(0) {
    std::memset(m_frameLen, 0, sizeof(m_frameLen));
}

AnalogCompute ::~AnalogCompute() {
    this->hwClose();
}

// ----------------------------------------------------------------------
// Hardware helpers
// ----------------------------------------------------------------------

U64 AnalogCompute ::nowNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<U64>(ts.tv_sec) * 1000000000ULL + static_cast<U64>(ts.tv_nsec);
}

void AnalogCompute ::pinMode(U32 pin, bool output) {
    const U32 reg = GPFSEL0 + pin / 10;
    const U32 shift = (pin % 10) * 3;
    U32 v = m_gpio[reg];
    v &= ~(7U << shift);
    if (output) {
        v |= (1U << shift);
    }
    m_gpio[reg] = v;
}

void AnalogCompute ::pinWrite(U32 pin, bool high) {
    m_gpio[high ? GPSET0 : GPCLR0] = (1U << pin);
}

bool AnalogCompute ::pinRead(U32 pin) const {
    return (m_gpio[GPLEV0] >> pin) & 1U;
}

bool AnalogCompute ::hwOpen(U32 spiHz) {
    this->hwClose();
    m_spiHz = (spiHz == 0) ? DEFAULT_SPI_HZ : spiHz;

    // --- GPIO via /dev/gpiomem (same register-level access as RPi.GPIO) ---
    m_gpioFd = ::open("/dev/gpiomem", O_RDWR | O_SYNC);
    if (m_gpioFd < 0) {
        this->log_WARNING_HI_HwError(Fw::LogStringArg("open /dev/gpiomem"), errno);
        return false;
    }
    void* map = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, m_gpioFd, 0);
    if (map == MAP_FAILED) {
        this->log_WARNING_HI_HwError(Fw::LogStringArg("mmap gpiomem"), errno);
        ::close(m_gpioFd);
        m_gpioFd = -1;
        return false;
    }
    m_gpio = static_cast<volatile U32*>(map);

    // Outputs: set level first, then switch to output (initial=LOW, as fpaa-tools)
    const U32 outs[] = {PIN_RESET_B, PIN_CE0, PIN_LCL_ACLK_EN, PIN_ACLK_SEL0, PIN_ACLK_SEL1};
    for (U32 p : outs) {
        this->pinWrite(p, false);
        this->pinMode(p, true);
    }
    const U32 ins[] = {PIN_CFGFLG_B, PIN_ACTIVATE, PIN_ERR_B, PIN_COMP};
    for (U32 p : ins) {
        this->pinMode(p, false);
    }
    // ACLK source = local 16 MHz oscillator: SEL1:SEL0 = 00, enable it
    this->pinWrite(PIN_ACLK_SEL0, false);
    this->pinWrite(PIN_ACLK_SEL1, false);
    this->pinWrite(PIN_LCL_ACLK_EN, true);
    this->pinWrite(PIN_CE0, false);  // CS held low for the whole session

    // --- SPI0, mode 0, no kernel chip select ---
    m_spiFd = ::open("/dev/spidev0.0", O_RDWR);
    if (m_spiFd < 0) {
        this->log_WARNING_HI_HwError(Fw::LogStringArg("open /dev/spidev0.0"), errno);
        this->hwClose();
        return false;
    }
    U32 mode32 = SPI_MODE_0 | SPI_NO_CS;
    if (ioctl(m_spiFd, SPI_IOC_WR_MODE32, &mode32) < 0) {
        U8 mode8 = static_cast<U8>(SPI_MODE_0 | SPI_NO_CS);
        if (ioctl(m_spiFd, SPI_IOC_WR_MODE, &mode8) < 0) {
            // fpaa-tools tolerates this too (CS already disabled by overlay)
            mode8 = SPI_MODE_0;
            (void)ioctl(m_spiFd, SPI_IOC_WR_MODE, &mode8);
        }
    }
    U8 bits = 8;
    U8 lsb = 0;
    (void)ioctl(m_spiFd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    (void)ioctl(m_spiFd, SPI_IOC_WR_LSB_FIRST, &lsb);
    if (ioctl(m_spiFd, SPI_IOC_WR_MAX_SPEED_HZ, &m_spiHz) < 0) {
        this->log_WARNING_HI_HwError(Fw::LogStringArg("set SPI speed"), errno);
        this->hwClose();
        return false;
    }
    return true;
}

// Releases file handles only. Pin levels are left as they are, so the
// configured chain keeps running (and keeps its analog state) after CLOSE.
void AnalogCompute ::hwClose() {
    if (m_spiFd >= 0) {
        ::close(m_spiFd);
        m_spiFd = -1;
    }
    if (m_gpio != nullptr) {
        munmap(const_cast<U32*>(m_gpio), 4096);
        m_gpio = nullptr;
    }
    if (m_gpioFd >= 0) {
        ::close(m_gpioFd);
        m_gpioFd = -1;
    }
}

bool AnalogCompute ::spiWrite(const U8* data, U32 len) {
    for (U32 off = 0; off < len; off += SPI_CHUNK) {
        const U32 n = (len - off < SPI_CHUNK) ? (len - off) : SPI_CHUNK;
        struct spi_ioc_transfer tr;
        std::memset(&tr, 0, sizeof(tr));
        tr.tx_buf = reinterpret_cast<unsigned long>(data + off);
        tr.rx_buf = 0;
        tr.len = n;
        tr.speed_hz = m_spiHz;
        tr.bits_per_word = 8;
        if (ioctl(m_spiFd, SPI_IOC_MESSAGE(1), &tr) < 0) {
            this->log_WARNING_HI_HwError(Fw::LogStringArg("SPI transfer"), errno);
            return false;
        }
    }
    return true;
}

// Send frame + one 0x00 dummy byte (clocks the config state machine through).
bool AnalogCompute ::sendSlot(U8 slot, U32& latencyUs) {
    U8 buf[MAX_FRAME + 1];
    const U32 len = m_frameLen[slot];
    std::memcpy(buf, m_frames[slot], len);
    buf[len] = 0x00;
    const U64 t0 = nowNs();
    const bool sent = this->spiWrite(buf, len + 1);
    latencyUs = static_cast<U32>((nowNs() - t0) / 1000ULL);
    const bool errOk = this->pinRead(PIN_ERR_B);

    m_updates++;
    this->tlmWrite_UpdatesSent(m_updates);
    this->tlmWrite_LastLatencyUs(latencyUs);
    this->tlmWrite_ErrOk(errOk);
    if (!sent || !errOk) {
        m_updateErrors++;
        this->tlmWrite_UpdateErrors(m_updateErrors);
        this->log_WARNING_HI_UpdateError(slot);
        return false;
    }
    this->log_ACTIVITY_HI_ContextSwitched(slot, len, latencyUs);
    return true;
}

void AnalogCompute ::housekeeping() {
    if (!this->isOpen()) {
        return;
    }
    this->tlmWrite_CompLevel(this->pinRead(PIN_COMP) ? 1 : 0);
    this->tlmWrite_ErrOk(this->pinRead(PIN_ERR_B));
    this->tlmWrite_Activate(this->pinRead(PIN_ACTIVATE));
}

// ----------------------------------------------------------------------
// Handlers
// ----------------------------------------------------------------------

void AnalogCompute ::run_handler(FwIndexType portNum, U32 context) {
    (void)portNum;
    (void)context;
    this->housekeeping();
}

void AnalogCompute ::OPEN_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U32 spiHz) {
    if (!this->hwOpen(spiHz)) {
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR);
        return;
    }
    this->log_ACTIVITY_HI_Opened(m_spiHz);
    this->housekeeping();
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void AnalogCompute ::PRIMARY_CONFIG_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, const Fw::CmdStringArg& path) {
    if (!this->isOpen()) {
        this->log_WARNING_LO_NotOpen();
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR);
        return;
    }
    // Parse .ahf text: one ASCII hex byte per line, blank lines skipped
    FILE* f = std::fopen(path.toChar(), "r");
    if (f == nullptr) {
        this->log_WARNING_HI_HwError(Fw::LogStringArg("open .ahf"), errno);
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR);
        return;
    }
    U32 n = 0;
    char line[64];
    bool bad = false;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        char* p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }
        if (*p == '\0') {
            continue;
        }
        unsigned int v = 0;
        if (std::sscanf(p, "%x", &v) != 1 || v > 0xFF || n >= MAX_PRIMARY) {
            bad = true;
            break;
        }
        m_primary[n++] = static_cast<U8>(v);
    }
    std::fclose(f);
    if (bad || n == 0) {
        this->log_WARNING_HI_HwError(Fw::LogStringArg("parse .ahf"), 0);
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::VALIDATION_ERROR);
        return;
    }

    // Reset chain, stream image in one burst, wait for auto-null
    this->pinWrite(PIN_RESET_B, false);
    sleepUs(RESET_LOW_US);
    this->pinWrite(PIN_RESET_B, true);
    sleepUs(RESET_SETTLE_US);
    const bool sent = this->spiWrite(m_primary, n);
    sleepUs(AUTONULL_US);
    const bool errOk = this->pinRead(PIN_ERR_B);
    const bool act = this->pinRead(PIN_ACTIVATE);
    this->housekeeping();

    if (!sent || !errOk || !act) {
        this->log_WARNING_HI_PrimaryFailed(Fw::LogStringArg(path.toChar()), errOk, act);
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR);
        return;
    }
    this->log_ACTIVITY_HI_PrimaryConfigured(Fw::LogStringArg(path.toChar()), n);
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void AnalogCompute ::LOAD_FRAME_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U8 slot, const Fw::CmdStringArg& path) {
    if (slot >= NUM_SLOTS) {
        this->log_WARNING_LO_BadSlot(slot);
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::VALIDATION_ERROR);
        return;
    }
    const I32 n = readFile(path.toChar(), m_frames[slot], MAX_FRAME);
    if (n <= 0 || m_frames[slot][0] != 0xD5) {
        m_frameLen[slot] = 0;
        this->log_WARNING_HI_HwError(Fw::LogStringArg("read frame (missing, empty, too big or no 0xD5 sync)"), errno);
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR);
        return;
    }
    m_frameLen[slot] = static_cast<U32>(n);
    this->log_ACTIVITY_LO_FrameLoaded(slot, m_frameLen[slot]);
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void AnalogCompute ::SEND_FRAME_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U8 slot) {
    if (!this->isOpen()) {
        this->log_WARNING_LO_NotOpen();
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR);
        return;
    }
    if (slot >= NUM_SLOTS || m_frameLen[slot] == 0) {
        this->log_WARNING_LO_BadSlot(slot);
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::VALIDATION_ERROR);
        return;
    }
    U32 lat = 0;
    const bool ok = this->sendSlot(slot, lat);
    this->cmdResponse_out(opCode, cmdSeq, ok ? Fw::CmdResponse::OK : Fw::CmdResponse::EXECUTION_ERROR);
}

void AnalogCompute ::SEND_AND_TIME_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U8 slot, U8 level, U32 timeoutMs) {
    if (!this->isOpen()) {
        this->log_WARNING_LO_NotOpen();
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR);
        return;
    }
    if (slot >= NUM_SLOTS || m_frameLen[slot] == 0 || level > 1) {
        this->log_WARNING_LO_BadSlot(slot);
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::VALIDATION_ERROR);
        return;
    }
    U32 lat = 0;
    if (!this->sendSlot(slot, lat)) {
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR);
        return;
    }
    // Busy-poll the comparator (register read, ~ns) from the end of the send
    const U64 t0 = nowNs();
    const U64 limit = static_cast<U64>(timeoutMs) * 1000000ULL;
    const bool want = (level != 0);
    U64 dt = 0;
    bool hit = false;
    while ((dt = nowNs() - t0) < limit) {
        if (this->pinRead(PIN_COMP) == want) {
            hit = true;
            break;
        }
    }
    this->tlmWrite_CompLevel(this->pinRead(PIN_COMP) ? 1 : 0);
    if (!hit) {
        m_timeouts++;
        this->tlmWrite_FlipTimeouts(m_timeouts);
        this->log_WARNING_HI_FlipTimeout(slot, level, timeoutMs);
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR);
        return;
    }
    const U32 us = static_cast<U32>(dt / 1000ULL);
    this->tlmWrite_LastFlipUs(us);
    this->log_ACTIVITY_HI_StateFlipped(slot, level, us);
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void AnalogCompute ::CLOSE_cmdHandler(FwOpcodeType opCode, U32 cmdSeq) {
    this->hwClose();
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

}  // namespace Amun
