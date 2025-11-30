#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "hw/PumpR07Protocol.h"

namespace recum12::hw {

class PumpInterfaceLvl3 {
public:
    using Byte  = PumpR07Protocol::Byte;
    using Frame = PumpR07Protocol::Frame;

    PumpInterfaceLvl3();
    ~PumpInterfaceLvl3();

    // --- Seri hat / RS-485 ayarları ---
    // Örn: "/dev/ttyUSB0"
    void setDevice(const std::string& device);
    const std::string& device() const noexcept { return m_device; }
    // RX döngüsü dışarıda kaldığında (ör: ayrı thread),
    // non-blocking poll ile mevcut byte'ları okuyup frame'leri çözer.
    // En az bir byte veya geçerli frame işlendiyse true döner.
    bool pollOnceRx();

    // Portu aç / kapa (Raspberry Pi üzerinde USB–RS485 dönüştürücü)
    bool open();
    void close();
    bool isOpen() const noexcept { return m_fd >= 0; }

    // --- Yüksek seviye komut yardımcıları ---
    // STATUS (CD1): DCC değeri ile durum sorgusu
    bool sendStatusPoll(std::uint8_t dcc);

    // MIN-POLL (heart-beat): 50 20 FA (→ 50 70 FA MIN-ACK beklenir)
    // Not: Adres olarak R07_DEFAULT_ADDR (0x50) kullanılır.
    bool sendMinPoll();

    // PRESET VOLUME (CD3): litre bazlı preset (0.1 .. 250.0 L aralığı)
    bool sendPresetVolume(double liters);

    // TOTAL COUNTERS (CD101 / 0x65)
    bool sendTotalCounters();

    // --- RX tarafı: dıştaki okuma döngüsü ham frame'i buraya verir ---
    void handleReceivedFrame(const Frame& frame);

    // --- Olay callback'leri (PumpR07Protocol'unkileri dışarı taşır) ---
    std::function<void(PumpState)>            onStatus;
    std::function<void(const FillInfo&)>      onFill;
    std::function<void(const TotalCounters&)> onTotals;
    std::function<void(const NozzleEvent&)>   onNozzle;

private:
    bool writeFrame(const Frame& frame);

    std::string     m_device;
    int             m_fd{-1};
    PumpR07Protocol m_proto;

    // RS-485 RX için biriktirilen ham byte'lar (frame kesme için).
    std::vector<Byte> m_rxBuffer;
};

} // namespace recum12::hw
