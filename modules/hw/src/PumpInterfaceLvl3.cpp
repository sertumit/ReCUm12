#include "hw/PumpInterfaceLvl3.h"
#include <cerrno>

#include <cstring>
#include <iostream>
#include <iomanip>
#include <sstream>

// Raspberry Pi / Linux seri port için POSIX API
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

namespace recum12::hw {

PumpInterfaceLvl3::PumpInterfaceLvl3()
    : m_device{}
    , m_fd{-1}
    , m_proto{}
{
    // PumpR07Protocol olaylarını L3 seviyesine forward et
    m_proto.onStatus = [this](PumpState st) {
        if (onStatus) {
            onStatus(st);
        }
    };
    m_proto.onFill = [this](const FillInfo& fi) {
        if (onFill) {
            onFill(fi);
        }
    };
    m_proto.onTotals = [this](const TotalCounters& tc) {
        if (onTotals) {
            onTotals(tc);
        }
    };
    m_proto.onNozzle = [this](const NozzleEvent& ev) {
        if (onNozzle) {
            onNozzle(ev);
        }
    };
}

PumpInterfaceLvl3::~PumpInterfaceLvl3()
{
    close();
}

void PumpInterfaceLvl3::setDevice(const std::string& device)
{
    m_device = device;
}

bool PumpInterfaceLvl3::open()
{
    if (m_fd >= 0) {
        return true; // zaten açık
    }
    if (m_device.empty()) {
        return false;
    }

    // Not: Raspberry Pi üzerinde /dev/ttyUSBx veya /dev/ttyAMA0 vb.
    int fd = ::open(m_device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        std::cerr << "[PumpL3] open fail " << m_device
                  << ": " << std::strerror(errno)
                  << std::endl;
        return false;
    }

    termios tio{};
    if (tcgetattr(fd, &tio) != 0) {
        ::close(fd);
        return false;
    }

    cfmakeraw(&tio);
    // Mepsan R07 için tipik ayar: 9600 8N1 (gerekirse daha sonra güncelleriz)
    cfsetispeed(&tio, B9600);
    cfsetospeed(&tio, B9600);

    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;      // 8 data bit
    tio.c_cflag &= ~CSTOPB;  // 1 stop bit

    // Simülatörün loguna göre: parity=O, 8b1 → ODD parity
    tio.c_cflag |= PARENB;
    tio.c_cflag |= PARODD;

    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        ::close(fd);
        return false;
    }

    m_fd = fd;
    std::cerr << "[PumpL3] open ok " << m_device
              << " fd=" << m_fd
              << std::endl;

    return true;
}

void PumpInterfaceLvl3::close()
{
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

bool PumpInterfaceLvl3::sendMinPoll()
{
    // Heart-beat: MIN-POLL (50 20 FA) gönderir.
    // Adres: R07_DEFAULT_ADDR (PumpR07Protocol.h içinde, 0x50)
    Frame fr = m_proto.makeMinPoll(R07_DEFAULT_ADDR);
    return writeFrame(fr);
}

bool PumpInterfaceLvl3::pollOnceRx()
{
    if (m_fd < 0) {
        return false;
    }

    bool any_read     = false;
    bool any_dispatched = false;

    Byte tmp[64];

    for (;;) {
        const ssize_t n = ::read(m_fd, tmp, sizeof(tmp));
        if (n > 0) {
            any_read = true;
            m_rxBuffer.insert(m_rxBuffer.end(), tmp, tmp + n);
        } else if (n == 0) {
            // Şimdilik "bağlantı kapandı" gibi durumları ayrı loglamıyoruz.
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                break;
            }
            // Diğer hatalarda döngüden çık; üst seviye tekrar open() deneyebilir.
            break;
        }
    }

    // R07 framing: TRAIL (0xFA) görüldükçe frame kes ve parse et.
    using recum12::hw::R07_TRAIL;
    auto toHex = [](const Frame& fr) {
        std::ostringstream oss;
        oss << std::uppercase << std::hex << std::setfill('0');
        for (std::size_t i = 0; i < fr.size(); ++i) {
            oss << std::setw(2)
                << static_cast<unsigned int>(fr[i]);
        }
        return oss.str();
    };

    for (;;) {
        auto it = std::find(m_rxBuffer.begin(), m_rxBuffer.end(), R07_TRAIL);
        if (it == m_rxBuffer.end()) {
            break;
        }

        const std::size_t frame_len =
            static_cast<std::size_t>(std::distance(m_rxBuffer.begin(), it)) + 1U;

        if (frame_len >= 3U) {
            Frame fr(m_rxBuffer.begin(), m_rxBuffer.begin() + frame_len);
            if (!fr.empty()) {
                std::cerr << "[PumpL3/RX] bytes=" << fr.size()
                          << " hex=" << toHex(fr)
                          << std::endl;

                m_proto.parseFrame(fr);
                any_dispatched = true;
            }
        }

        // Bu frame'i buffer'dan çıkar, sonraki adayları aramaya devam et.
        m_rxBuffer.erase(m_rxBuffer.begin(), m_rxBuffer.begin() + frame_len);
    }

    return any_read || any_dispatched;
}

bool PumpInterfaceLvl3::writeFrame(const Frame& frame)
{
    if (m_fd < 0 || frame.empty()) {
        return false;
    }
    // Debug: TX frame'i hex olarak logla (Python controller benzeri).
    auto toHex = [](const Frame& fr) {
        std::ostringstream oss;
        oss << std::uppercase << std::hex << std::setfill('0');
        for (std::size_t i = 0; i < fr.size(); ++i) {
            oss << std::setw(2)
                << static_cast<unsigned int>(fr[i]);
        }
        return oss.str();
    };

    std::cerr << "[PumpL3/TX] bytes=" << frame.size()
              << " hex=" << toHex(frame)
              << std::endl;

    const std::uint8_t* data = frame.data();
    std::size_t         left = frame.size();

    while (left > 0) {
        const ssize_t n = ::write(m_fd, data, left);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            break;
        }
        data += static_cast<std::size_t>(n);
        left -= static_cast<std::size_t>(n);
    }
    return (left == 0);
}

bool PumpInterfaceLvl3::sendStatusPoll(std::uint8_t dcc)
{
    Frame fr = m_proto.makeStatusPollFrame(dcc);
    return writeFrame(fr);
}

bool PumpInterfaceLvl3::sendPresetVolume(double liters)
{
    Frame fr = m_proto.makePresetVolumeFrame(liters);
    return writeFrame(fr);
}

bool PumpInterfaceLvl3::sendTotalCounters()
{
    Frame fr = m_proto.makeTotalCountersFrame();
    return writeFrame(fr);
}

void PumpInterfaceLvl3::handleReceivedFrame(const Frame& frame)
{
    if (frame.empty()) {
        return;
    }
    m_proto.parseFrame(frame);
}

}
