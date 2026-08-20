// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/usb/HidrawGateway.h"

#include <linux/hidraw.h>
#include <linux/input.h>
#include <sys/ioctl.h>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace dish::source::usb {

namespace {

constexpr std::uint16_t kUsagePageGenericDesktop = 0x01;
constexpr std::uint16_t kUsageJoystick = 0x04;
constexpr std::uint16_t kUsageGamepad = 0x05;
// The Steam Controller's game interface is vendor-defined HID with no gamepad
// usages, so it is admitted by model rather than by shape.
constexpr std::uint16_t kUsagePageVendor = 0xFF00;

// Bus 0003 in the HID_ID triple; 0005 is Bluetooth.
constexpr std::uint32_t kBusUsb = BUS_USB;

// Xbox pads reach userspace through xpad as evdev only, so they never appear
// here. The VID is skipped anyway so a Microsoft HID peripheral that does
// publish a gamepad collection is never fought over.
constexpr int kVidMicrosoft = 0x045E;

// Fast-lane vendors worth auto-claiming Direct: Sony, Nintendo, 8BitDo. Models
// from the known-model table (PDP Switch pads, the Steam Controller) are
// reached only through an explicit Direct pick — the Steam Controller claim
// reconfigures a device its owner may be using as a desktop mouse.
constexpr int kVidSony = 0x054C;
constexpr int kVidNintendo = 0x057E;
constexpr int kVid8BitDo = 0x2DC8;

// Feature-report writes retry the way SDL and hid-steam retry EPIPE: the
// wireless dongle under load fails transiently, not terminally. Capped so an
// unresponsive device still finishes inside the manager's transition timeout.
constexpr int kFeatureReportAttempts = 25;
constexpr int kFeatureReportRetryMs = 20;

// Long enough that an idle pad costs almost nothing, short enough that a
// release observes running=false promptly.
constexpr int kReadPollTimeoutMs = 100;

constexpr std::size_t kMaxFeatureLen = 128;

std::string readSysfsLine(const std::string& path) {
    std::ifstream in(path);
    if (!in) { return {}; }
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
    }
    return line;
}

// Chunked rather than the istreambuf_iterator pair: GCC 13 cannot see through
// that constructor's inlined sbumpc and reports -Wnull-dereference on
// libstdc++'s own gptr(), which is -Werror here. sysfs files are small, so one
// resize-and-read loop costs nothing.
std::string readSysfsText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { return {}; }
    std::string out;
    std::array<char, 4096> buf{};
    while (in.read(buf.data(), static_cast<std::streamsize>(buf.size())) || in.gcount() > 0) {
        out.append(buf.data(), static_cast<std::size_t>(in.gcount()));
        if (!in) { break; }
    }
    return out;
}

std::optional<int> parseHex(const std::string& s) {
    if (s.empty()) { return std::nullopt; }
    int value = 0;
    const auto* first = s.data();
    const auto* last = s.data() + s.size();
    const auto res = std::from_chars(first, last, value, 16);
    if (res.ec != std::errc{} || res.ptr != last) { return std::nullopt; }
    return value;
}

// The top-level application collection's usage page + usage, which is what the
// admission rule keys on. Walks short items only; a long item (0xFE) ends the
// scan because nothing we admit uses one.
bool topLevelUsage(const std::uint8_t* desc, std::size_t len, std::uint16_t& outPage,
                   std::uint16_t& outUsage) {
    std::uint32_t page = 0;
    std::uint32_t usage = 0;
    bool sawUsage = false;
    for (std::size_t i = 0; i < len;) {
        const std::uint8_t prefix = desc[i];
        if (prefix == 0xFE) { return false; }
        const std::uint8_t tag = prefix & 0xFC;
        std::uint8_t size = prefix & 0x03;
        if (size == 3) { size = 4; }
        if (i + 1 + size > len) { return false; }
        std::uint32_t data = 0;
        for (std::uint8_t b = 0; b < size; b++) {
            data |= static_cast<std::uint32_t>(desc[i + 1 + b]) << (8 * b);
        }
        if (tag == 0x04) { // Usage Page (global)
            page = data;
        } else if (tag == 0x08 && !sawUsage) { // Usage (local)
            usage = data;
            sawUsage = true;
        } else if (tag == 0xA0) { // Collection
            if (data == 0x01) {   // Application
                outPage = static_cast<std::uint16_t>(page);
                outUsage = static_cast<std::uint16_t>(usage);
                return true;
            }
        }
        i += 1u + size;
    }
    return false;
}

bool collectionMatchesParser(std::uint16_t page, std::uint16_t usage,
                             input::usbparse::HidParser parser) {
    if (parser == input::usbparse::HidParser::SteamController) { return page == kUsagePageVendor; }
    return page == kUsagePageGenericDesktop && (usage == kUsageGamepad || usage == kUsageJoystick);
}

// report_descriptor is mode 0444 and holds the same bytes HIDIOCGRDESC returns.
// Anything past HID_MAX_DESCRIPTOR_SIZE is not one, since the kernel refuses to
// register a descriptor that large.
constexpr std::size_t kMaxReportDescriptor = 4096;

bool readReportDescriptor(const std::string& path, std::vector<std::uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { return false; }
    out.clear();
    char byte = 0;
    while (in.get(byte)) {
        if (out.size() == kMaxReportDescriptor) { return false; }
        out.push_back(static_cast<std::uint8_t>(byte));
    }
    return !out.empty();
}

// The USB interface directory backing a hidraw node, e.g.
// /sys/class/hidraw/hidraw3/device/.. — empty when the node is not USB-backed.
std::string usbInterfaceDir(const std::string& hidrawName) {
    std::string base = "/sys/class/hidraw/" + hidrawName + "/device/..";
    if (readSysfsLine(base + "/bInterfaceNumber").empty()) { return {}; }
    return base;
}

// bInterval and wMaxPacketSize off the interrupt-IN endpoint, plus whether an
// OUT endpoint exists (which is what gates rumble). Defaults stay when the node
// has no USB parent to read.
void readEndpointInfo(const std::string& ifaceDir, UsbDeviceInfo& info) {
    if (ifaceDir.empty()) { return; }
    DIR* dir = ::opendir(ifaceDir.c_str());
    if (dir == nullptr) { return; }
    while (dirent* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.rfind("ep_", 0) != 0) { continue; }
        const auto addr = parseHex(name.substr(3));
        if (!addr) { continue; }
        if ((*addr & 0x80) != 0) {
            std::string endpointDir = ifaceDir;
            endpointDir += "/";
            endpointDir += name;
            if (const auto mp = parseHex(readSysfsLine(endpointDir + "/wMaxPacketSize"))) {
                info.endpointInMaxPacket = *mp;
            }
            const std::string interval = readSysfsLine(endpointDir + "/bInterval");
            if (const auto iv = parseHex(interval)) { info.endpointInInterval = *iv; }
        } else {
            info.hasOutEndpoint = true;
        }
    }
    ::closedir(dir);
}

bool sendSteamFeature(int fd, int featureLen, const std::uint8_t* data, std::size_t len) {
    std::array<std::uint8_t, kMaxFeatureLen> buf{};
    if (featureLen <= 0 || static_cast<std::size_t>(featureLen) > buf.size() ||
        len + 1 > static_cast<std::size_t>(featureLen)) {
        return false;
    }
    std::memcpy(buf.data() + 1, data, len);
    for (int attempt = 0; attempt < kFeatureReportAttempts; attempt++) {
        if (::ioctl(fd, HIDIOCSFEATURE(static_cast<unsigned>(featureLen)), buf.data()) >= 0) {
            return true;
        }
        ::usleep(kFeatureReportRetryMs * 1000);
    }
    return false;
}

// Restore is best-effort: a pad that is gone can no longer be restored, and
// failing the release over it would strand the claim.
bool runSteamConfig(int fd, int featureLen, input::usbparse::SteamConfig stage) {
    std::array<std::uint8_t, 16> pkt{};
    for (int i = 0;; i++) {
        const std::size_t n =
            input::usbparse::buildSteamConfigPacket(stage, i, pkt.data(), pkt.size());
        if (n == 0) { break; }
        if (!sendSteamFeature(fd, featureLen, pkt.data(), n)) { return false; }
    }
    return true;
}

// hidraw has no feature-length query, so the descriptor's largest feature
// report is the ceiling. The Steam Controller's is 65 (id byte + 64 payload).
constexpr int kSteamFeatureLen = 65;

std::vector<std::string> hidrawNodes() {
    std::vector<std::string> out;
    DIR* dir = ::opendir("/sys/class/hidraw");
    if (dir == nullptr) { return out; }
    while (dirent* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.rfind("hidraw", 0) == 0) { out.push_back(name); }
    }
    ::closedir(dir);
    std::sort(out.begin(), out.end());
    return out;
}

struct ProbedNode {
    std::string node;
    std::string name;
    int vendorId = 0;
    int productId = 0;
    std::uint16_t usagePage = 0;
    std::uint16_t usage = 0;
    std::vector<std::uint8_t> descriptor;
};

// Probes sysfs, never the node: /dev/hidraw* is 0600 root:root until the udev
// rule lands, so opening it here would drop the device from enumeration
// entirely instead of letting the claim fail PermissionDenied. Both attributes
// read here are world-readable, and HID_ID carries the same bus/VID/PID
// HIDIOCGRAWINFO would have returned.
//
// A Bluetooth-connected pad is skipped by the same BUS_USB rule as before: the
// per-model decoders parse the USB report layout, and the BT layout differs (a
// DS4 streams the short 0x01 report until a feature-report handshake).
std::optional<ProbedNode> probe(const std::string& hidrawName) {
    const std::string sysDir = "/sys/class/hidraw/" + hidrawName + "/device";
    const std::string uevent = readSysfsText(sysDir + "/uevent");
    const auto ids = detail::parseHidIds(uevent);
    if (!ids || ids->bus != kBusUsb) { return std::nullopt; }

    ProbedNode probed;
    probed.node = "/dev/" + hidrawName;
    // HID_NAME is the same hid->name string HIDIOCGRAWNAME copies out.
    probed.name = detail::ueventValue(uevent, "HID_NAME");
    probed.vendorId = static_cast<std::uint16_t>(ids->vendorId);
    probed.productId = static_cast<std::uint16_t>(ids->productId);
    if (!readReportDescriptor(sysDir + "/report_descriptor", probed.descriptor) ||
        !topLevelUsage(probed.descriptor.data(), probed.descriptor.size(), probed.usagePage,
                       probed.usage)) {
        return std::nullopt;
    }
    return probed;
}

} // namespace

HidrawGateway::Claimed::~Claimed() = default;

HidrawGateway::HidrawGateway() = default;

HidrawGateway::~HidrawGateway() {
    // Drained in place rather than snapshotting ids into a vector first: a
    // destructor is implicitly noexcept, so that vector's growth was a live
    // std::terminate path, and bailing out of the drain would leave joinable
    // threads in claimed_ — which terminates just the same. Nothing in this
    // loop allocates. releaseClaim re-takes mtx_, so it is dropped before each
    // call and the reader is joined with the mutex free.
    for (;;) {
        int syntheticId = 0;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (claimed_.empty()) { break; }
            syntheticId = claimed_.begin()->first;
        }
        releaseClaim(syntheticId);
    }
}

bool HidrawGateway::isKnownFastLaneModel(int vendorId, int /*productId*/) const {
    return vendorId == kVidSony || vendorId == kVidNintendo || vendorId == kVid8BitDo;
}

std::vector<UsbDeviceInfo> HidrawGateway::enumerate() {
    std::vector<UsbDeviceInfo> out;
    for (const std::string& hidrawName : hidrawNodes()) {
        const auto probed = probe(hidrawName);
        if (!probed) { continue; }
        if (probed->vendorId == kVidMicrosoft) { continue; }

        const auto parser = input::usbparse::parserForDevice(probed->vendorId, probed->productId);
        if (!collectionMatchesParser(probed->usagePage, probed->usage, parser)) { continue; }

        UsbDeviceInfo info;
        info.vendorId = probed->vendorId;
        info.productId = probed->productId;
        const auto* model = input::usbparse::lookupKnownModel(info.vendorId, info.productId);
        if (model != nullptr) {
            info.name = model->name;
        } else {
            info.name = probed->name.empty() ? probed->node : probed->name;
        }

        const std::string ifaceDir = usbInterfaceDir(hidrawName);
        if (const auto num = parseHex(readSysfsLine(ifaceDir + "/bInterfaceNumber"))) {
            info.interfaceNumber = *num;
        }
        readEndpointInfo(ifaceDir, info);
        info.hasImu = input::usbparse::parserHasImu(parser);
        out.push_back(std::move(info));
    }
    return out;
}

ClaimResult HidrawGateway::claim(const UsbDeviceInfo& device,
                                 std::function<void(const UsbReport&)> onReport) {
    const auto parser = input::usbparse::parserForDevice(device.vendorId, device.productId);

    // enumerate() returned descriptors, not handles, so the claim re-finds the
    // node by VID:PID and owns the open. A model can expose several collections
    // under one VID:PID (the Steam Controller's keyboard and mouse ride beside
    // its game interface); only the one the parser decodes is the claim.
    int fd = -1;
    std::vector<std::uint8_t> descriptor;
    bool sawDevice = false;
    bool sawPermissionDenied = false;
    for (const std::string& hidrawName : hidrawNodes()) {
        const auto probed = probe(hidrawName);
        if (!probed || probed->vendorId != device.vendorId ||
            probed->productId != device.productId) {
            continue;
        }
        sawDevice = true;
        if (!collectionMatchesParser(probed->usagePage, probed->usage, parser)) { continue; }
        const int opened = ::open(probed->node.c_str(), O_RDWR | O_CLOEXEC);
        if (opened < 0) {
            if (errno == EACCES || errno == EPERM) { sawPermissionDenied = true; }
            continue;
        }
        fd = opened;
        descriptor = probed->descriptor;
        break;
    }

    if (fd < 0) {
        // A device we saw but could not open for write is almost always the
        // missing udev rule, and the distinction drives different UI copy. Any
        // other failure folds to Busy; the framework was never stolen, so the
        // SDL path stays usable either way.
        const auto reason = (sawDevice && sawPermissionDenied)
                                ? reducer::DirectClaimFailure::PermissionDenied
                                : reducer::DirectClaimFailure::Busy;
        return ClaimResult::fail(reason, /*frameworkStolen=*/false);
    }

    // The Steam Controller ships emulating a keyboard and mouse; quiet mode
    // switches that off and enables the IMU. This is the one family whose init
    // persistently reconfigures the device, so a failure restores the defaults
    // before giving up — a partly-applied init must never strand the pad mute.
    int featureReportLen = 0;
    if (parser == input::usbparse::HidParser::SteamController) {
        featureReportLen = kSteamFeatureLen;
        if (!runSteamConfig(fd, featureReportLen, input::usbparse::SteamConfig::Quiet)) {
            runSteamConfig(fd, featureReportLen, input::usbparse::SteamConfig::Restore);
            ::close(fd);
            return ClaimResult::fail(reducer::DirectClaimFailure::InitFailed,
                                     /*frameworkStolen=*/false);
        }
    }

    const int syntheticId = nextSyntheticId_.fetch_sub(1);
    auto claimed = std::make_unique<Claimed>();
    claimed->node = device.name;
    claimed->fd = fd;
    claimed->onReport = std::move(onReport);
    claimed->vendorId = device.vendorId;
    claimed->productId = device.productId;
    claimed->parser = parser;
    claimed->featureReportLen = featureReportLen;
    if (parser == input::usbparse::HidParser::GenericHid) {
        // The descriptor-derived field map replaces the fixed-offset guess
        // wherever the collection declares real usages; the guess stays as the
        // fallback for a descriptor we cannot parse.
        input::usbhid::parseReportDescriptor(descriptor.data(), descriptor.size(), claimed->layout);
        // From the model catalog, not the descriptor: the PDP Switch pads
        // declare their buttons in Switch usage order, and the parse above just
        // reset this to false.
        claimed->layout.switchOrderButtons =
            input::usbparse::buttonOrderForDevice(device.vendorId, device.productId) ==
            input::usbparse::ButtonOrder::Switch;
    }
    claimed->running.store(true);
    Claimed* raw = claimed.get();
    {
        std::lock_guard<std::mutex> lock(mtx_);
        claimed_.emplace(syntheticId, std::move(claimed));
    }
    raw->reader = std::thread([this, raw] { readLoop(raw); });
    return ClaimResult::success(syntheticId);
}

void HidrawGateway::readLoop(Claimed* c) {
    // Plain C++ on its own thread, no Qt and no allocation per report: the read
    // buffer and the ParsedReport scratch live on this stack and are reused
    // every iteration, and the decoders are pure functions over them that
    // mutate the device's stick auto-range state in place.
    std::array<std::uint8_t, 128> buf{};
    pollfd pfd{};
    pfd.fd = c->fd;
    pfd.events = POLLIN;

    while (c->running.load()) {
        const int ready = ::poll(&pfd, 1, kReadPollTimeoutMs);
        if (ready < 0) {
            if (errno == EINTR) { continue; }
            break;
        }
        if (ready == 0) { continue; }
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) { break; }

        const ssize_t got = ::read(c->fd, buf.data(), buf.size());
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN) { continue; }
            break;
        }
        if (got == 0) { continue; }
        c->completions.fetch_add(1);

        const std::uint8_t* data = buf.data();
        auto len = static_cast<std::size_t>(got);
        if (c->parser == input::usbparse::HidParser::SteamController) {
            // Dongle connect/disconnect events interleave with input. A
            // returning pad has rebooted (settings gone), so quiet mode is
            // re-applied; a departing pad's last input must not stay latched.
            const auto ev = input::usbparse::checkWirelessEvent(c->parser, data, len);
            if (ev == input::usbparse::WirelessEvent::Connect) {
                runSteamConfig(c->fd, c->featureReportLen, input::usbparse::SteamConfig::Quiet);
                continue;
            }
            if (ev == input::usbparse::WirelessEvent::Disconnect) {
                if (c->onReport) { c->onReport(UsbReport{}); }
                continue;
            }
        }

        input::usbparse::ParsedReport parsed{};
        const bool decoded =
            c->layout.valid
                ? input::usbhid::decodeFromLayout(data, len, parsed, c->layout)
                : input::usbparse::decodeReport(c->parser, data, len, parsed, c->sticks);
        if (!decoded) { continue; }

        UsbReport report{};
        report.wButtons = parsed.wButtons;
        report.lt = parsed.lt;
        report.rt = parsed.rt;
        report.lx = parsed.lx;
        report.ly = parsed.ly;
        report.rx = parsed.rx;
        report.ry = parsed.ry;
        report.motionValid = parsed.motionValid;
        report.gyroX = parsed.gyroX;
        report.gyroY = parsed.gyroY;
        report.gyroZ = parsed.gyroZ;
        report.accelX = parsed.accelX;
        report.accelY = parsed.accelY;
        report.accelZ = parsed.accelZ;
        report.touchpadValid = parsed.touchpadValid;
        report.finger0Active = parsed.finger0Active;
        report.finger0Id = parsed.finger0Id;
        report.finger0X = parsed.finger0X;
        report.finger0Y = parsed.finger0Y;
        report.finger1Active = parsed.finger1Active;
        report.finger1Id = parsed.finger1Id;
        report.finger1X = parsed.finger1X;
        report.finger1Y = parsed.finger1Y;
        report.touchpadButton = parsed.touchpadButton;
        if (c->onReport) { c->onReport(report); }
    }
}

void HidrawGateway::releaseClaim(int syntheticId) {
    std::unique_ptr<Claimed> claimed;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const auto it = claimed_.find(syntheticId);
        if (it == claimed_.end()) { return; }
        claimed = std::move(it->second);
        claimed_.erase(it);
    }
    claimed->running.store(false);
    if (claimed->reader.joinable()) { claimed->reader.join(); }
    if (claimed->fd >= 0) {
        // Quiet mode persists on the device, so every release path restores the
        // stand-alone keyboard/mouse identity before the fd closes; skipping it
        // would hand back a controller that no longer works as a desktop mouse.
        if (claimed->parser == input::usbparse::HidParser::SteamController) {
            runSteamConfig(claimed->fd, claimed->featureReportLen,
                           input::usbparse::SteamConfig::Restore);
        }
        ::close(claimed->fd);
        claimed->fd = -1;
    }
}

std::int64_t HidrawGateway::completionCount(int syntheticId) const {
    std::lock_guard<std::mutex> lock(mtx_);
    const auto it = claimed_.find(syntheticId);
    if (it == claimed_.end()) { return 0; }
    return it->second->completions.load();
}

} // namespace dish::source::usb
