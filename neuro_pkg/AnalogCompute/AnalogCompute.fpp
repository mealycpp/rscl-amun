module Amun {

  @ PIKA HAT manager: primary configuration, in-place partial reconfiguration
  @ of the AN231E04 chain, and comparator readout (FPAA0 IO5P -> GPIO16).
  active component AnalogCompute {

    # ------------------------------------------------------------------
    # Ports
    # ------------------------------------------------------------------

    @ 1 Hz housekeeping: samples ERR_B, ACTIVATE and the comparator
    async input port run: Svc.Sched drop

    # ------------------------------------------------------------------
    # Commands
    # ------------------------------------------------------------------

    @ Claim SPI0 + PIKA control lines; chain is held in reset until PRIMARY_CONFIG
    async command OPEN(
        spiHz: U32 @< SPI clock in Hz (0 = 32 MHz, the fpaa-tools default)
      )

    @ Reset the chain and load a full primary image (.ahf text). Destroys all analog state.
    async command PRIMARY_CONFIG(
        path: string size 200 @< .ahf file
      )

    @ Load a binary partial-reconfiguration frame into a slot
    async command LOAD_FRAME(
        $slot: U8 @< slot 0..15
        path: string size 200 @< .bin frame file
      )

    @ Send the frame in a slot: in-place partial reconfiguration, no reset
    async command SEND_FRAME(
        $slot: U8 @< slot 0..15
      )

    @ Send the frame in a slot, then time the comparator until it reads `level`
    async command SEND_AND_TIME(
        $slot: U8 @< slot 0..15
        level: U8 @< comparator level to wait for (0/1)
        timeoutMs: U32 @< give up after this many ms
      )

    @ Release SPI and GPIO
    async command CLOSE

    # ------------------------------------------------------------------
    # Events
    # ------------------------------------------------------------------

    event Opened(spiHz: U32) \
      severity activity high \
      format "PIKA opened, SPI {} Hz, chain held in reset"

    event HwError(what: string size 80, err: I32) \
      severity warning high \
      format "Hardware error: {} (errno {})"

    event NotOpen \
      severity warning low \
      format "PIKA not open; send OPEN first"

    event PrimaryConfigured(path: string size 200, nBytes: U32) \
      severity activity high \
      format "Primary configuration {} ({} bytes): ERR_B high, ACTIVATE high"

    event PrimaryFailed(path: string size 200, errOk: bool, activate: bool) \
      severity warning high \
      format "Primary configuration {} failed: errOk={} activate={}"

    event FrameLoaded($slot: U8, nBytes: U32) \
      severity activity low \
      format "Frame slot {} loaded ({} bytes)"

    event BadSlot($slot: U8) \
      severity warning low \
      format "Slot {} is empty or out of range"

    event ContextSwitched($slot: U8, nBytes: U32, latencyUs: U32) \
      severity activity high \
      format "Context switched in place: slot {} ({} bytes) in {} us, ERR_B high"

    event UpdateError($slot: U8) \
      severity warning high \
      format "ERR_B low after update frame in slot {}"

    event StateFlipped($slot: U8, level: U8, tFlipUs: U32) \
      severity activity high \
      format "After slot {}: comparator reached {} in {} us"

    event FlipTimeout($slot: U8, level: U8, timeoutMs: U32) \
      severity warning high \
      format "After slot {}: comparator did not reach {} within {} ms"

    # ------------------------------------------------------------------
    # Telemetry
    # ------------------------------------------------------------------

    telemetry CompLevel: U8
    telemetry ErrOk: bool
    telemetry Activate: bool
    telemetry UpdatesSent: U32 update on change
    telemetry UpdateErrors: U32 update on change
    telemetry FlipTimeouts: U32 update on change
    telemetry LastLatencyUs: U32
    telemetry LastFlipUs: U32

    # ------------------------------------------------------------------
    # Standard ports
    # ------------------------------------------------------------------

    @ Port for requesting the current time
    time get port timeCaller

    @ Enables command handling
    import Fw.Command

    @ Enables event handling
    import Fw.Event

    @ Enables telemetry channels handling
    import Fw.Channel
  }
}
