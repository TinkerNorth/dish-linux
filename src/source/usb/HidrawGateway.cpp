// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/usb/HidrawGateway.h"

#include "core/input/UsbOutputReports.h"
#include "source/system/ThreadPriority.h"

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

// Long enough that an idle pad costs almost nothing, short enough that a
// release observes running=false promptly.
constexpr int kReadPollTimeoutMs = 100;

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

// The USB interface a hidraw node belongs to, or -1 when it has none to read.
int interfaceNumberOf(const std::string& hidrawName) {
    const std::string ifaceDir = usbInterfaceDir(hidrawName);
    if (ifaceDir.empty()) { return -1; }
    return parseHex(readSysfsLine(ifaceDir + "/bInterfaceNumber")).value_or(-1);
}

// Whether the node carries a pad: every node does but a Steam Controller receiver's keyboard,
// comms and pogo-pin interfaces, whose pads ride its slots only.
bool nodeCarriesPad(const std::string& hidrawName, int vendorId, int productId) {
    if (!input::usbparse::isReceiver(vendorId, productId)) { return true; }
    return input::usbparse::interfaceCarriesPad(vendorId, productId, interfaceNumberOf(hidrawName));
}

// A receiver's claim opens the slot it was asked for, because each slot is a pad of its own; any
// other model's opens the first node that carries its pad.
bool nodeIsTheClaimedPad(const std::string& hidrawName, const UsbDeviceInfo& device) {
    if (!input::usbparse::isReceiver(device.vendorId, device.productId)) { return true; }
    return interfaceNumberOf(hidrawName) == device.interfaceNumber;
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

// A 2015 receiver's slot answers with the connect or disconnect event for its controller, on
// the read loop. The one feature write a claim makes; a pad's settings are its output worker's.
bool askSteamReceiverSlot(HidReportWriter& writer) {
    std::array<std::uint8_t, input::usbparse::kSteamFeatureReportBytes> report{};
    const std::size_t n =
        input::usbparse::buildSteamReceiverStateReport(report.data(), report.size());
    const int attempts =
        input::usbparse::configWriteAttempts(input::usbparse::HidParser::SteamController);
    return writeFeatureWithRetry(writer, report.data(), n, attempts);
}

// The DualSense's firmware-info feature report, which picks its rumble mode. HIDIOCGFEATURE answers
// with the length it read, report id included, which the parse checks; an unanswered read keeps
// the pad on the original mode.
input::usbout::Ds5FirmwareInfo readDs5FirmwareInfo(int fd) {
    std::array<std::uint8_t, input::usbout::kDs5FirmwareInfoReportBytes> buf{};
    buf[0] = input::usbout::kDs5FirmwareInfoReportId;
    const int got = ::ioctl(fd, HIDIOCGFEATURE(static_cast<unsigned>(buf.size())), buf.data());
    if (got < 0) { return input::usbout::Ds5FirmwareInfo{}; }
    return input::usbout::parseDs5FirmwareInfo(buf.data(), static_cast<std::size_t>(got));
}

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
    // The USB device's `product` attribute (iProduct) first: the audio stack
    // derives its card and node names from that same descriptor string, and
    // the pad-to-endpoint matcher (core/audio/PadAudioMatcher.h) keys on
    // exactly that shared fact — it is also what Windows and android
    // enumerate, so the pad shows one name across clients. HID_NAME would not
    // do for either job: the kernel builds it as "<manufacturer> <product>",
    // and the longer string is contained in no endpoint name. It stays as the
    // fallback (the same hid->name string HIDIOCGRAWNAME copies out) for a
    // device with no readable product string.
    probed.name = readSysfsLine(sysDir + "/../../product");
    if (probed.name.empty()) { probed.name = detail::ueventValue(uevent, "HID_NAME"); }
    probed.vendorId = static_cast<std::uint16_t>(ids->vendorId);
    probed.productId = static_cast<std::uint16_t>(ids->productId);
    // Admission is decided per parser at the call sites, so probe keeps the
    // descriptor and only requires that it declares a top-level collection.
    if (!readReportDescriptor(sysDir + "/report_descriptor", probed.descriptor) ||
        detail::topLevelUsages(probed.descriptor.data(), probed.descriptor.size()).empty()) {
        return std::nullopt;
    }
    return probed;
}

// What one wait on the device came back with. Idle covers a poll timeout, an interrupted call and
// a zero-length read: none is an error and none carries a report. Stop is a device that has gone.
enum class ReadOutcome : std::uint8_t { Report, Idle, Stop };

ReadOutcome readOneReport(pollfd& pfd, std::array<std::uint8_t, 128>& buf, std::size_t& got) {
    got = 0;
    const int ready = ::poll(&pfd, 1, kReadPollTimeoutMs);
    if (ready < 0) { return errno == EINTR ? ReadOutcome::Idle : ReadOutcome::Stop; }
    if (ready == 0) { return ReadOutcome::Idle; }
    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) { return ReadOutcome::Stop; }

    const ssize_t n = ::read(pfd.fd, buf.data(), buf.size());
    if (n < 0) {
        return (errno == EINTR || errno == EAGAIN) ? ReadOutcome::Idle : ReadOutcome::Stop;
    }
    if (n == 0) { return ReadOutcome::Idle; }
    got = static_cast<std::size_t>(n);
    return ReadOutcome::Report;
}

// Straight field copy: ParsedReport is the decoder's shape and UsbReport is the gateway's.
// micMuted is the latch rather than the button: what the wire's kXusbMicMute bit carries, mirrored
// up so the driver can edge-detect it.
UsbReport toUsbReport(const input::usbparse::ParsedReport& parsed, const bool micMuted) {
    UsbReport report{};
    report.wButtons = parsed.wButtons;
    report.extButtons = parsed.extButtons;
    report.micMuted = micMuted;
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
    report.batteryValid = parsed.batteryValid;
    report.batteryLevel = parsed.batteryLevel;
    report.batteryStatus = parsed.batteryStatus;
    return report;
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
            std::lock_guard<thread::TrackedMutex> lock(mtx_);
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
        if (!detail::admitsParser(probed->descriptor.data(), probed->descriptor.size(), parser)) {
            continue;
        }
        if (!nodeCarriesPad(hidrawName, probed->vendorId, probed->productId)) { continue; }

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
        if (!detail::admitsParser(probed->descriptor.data(), probed->descriptor.size(), parser)) {
            continue;
        }
        if (!nodeIsTheClaimedPad(hidrawName, device)) { continue; }
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

    // A Steam Controller's settings (lizard mode off, the IMU on) are its output
    // worker's to put on the pad and to take off again, so the claim writes
    // nothing to a pad. A 2015 receiver's slot is asked what it holds, the way
    // SDL's InitDevice and hid-steam's probe start a receiver; the answer is its
    // controller's connect or disconnect event, on the read loop.
    const bool isSteam2015 = parser == input::usbparse::HidParser::SteamController;
    const bool receiverSlot = input::usbparse::isReceiver(device.vendorId, device.productId);
    auto claimed = std::make_unique<Claimed>();
    if (isSteam2015 && receiverSlot) {
        FdReportWriter writer(fd, &claimed->writeMtx);
        if (!askSteamReceiverSlot(writer)) {
            ::close(fd);
            return ClaimResult::fail(reducer::DirectClaimFailure::InitFailed,
                                     /*frameworkStolen=*/false);
        }
    }

    // Read once at the claim, the way hid-playstation reads it at probe.
    const input::usbout::Ds5FirmwareInfo ds5Firmware =
        parser == input::usbparse::HidParser::DualSense ? readDs5FirmwareInfo(fd)
                                                        : input::usbout::Ds5FirmwareInfo{};

    const int syntheticId = nextSyntheticId_.fetch_sub(1);
    claimed->node = device.name;
    claimed->fd = fd;
    claimed->onReport = std::move(onReport);
    claimed->vendorId = device.vendorId;
    claimed->productId = device.productId;
    claimed->parser = parser;
    claimed->linked.store(!receiverSlot);
    if (parser == input::usbparse::HidParser::GenericHid) {
        // The descriptor-derived field map replaces the fixed-offset guess
        // wherever the collection declares real usages; the guess stays as the
        // fallback for a descriptor we cannot parse.
        input::usbhid::parseReportDescriptor(descriptor.data(), descriptor.size(), claimed->layout);
        // From the model catalog, not the descriptor, which the parse above just
        // reset to the western order.
        claimed->layout.order =
            input::usbparse::buttonOrderForDevice(device.vendorId, device.productId);
    }
    claimed->running.store(true);
    Claimed* raw = claimed.get();
    {
        std::lock_guard<thread::TrackedMutex> lock(mtx_);
        claimed_.emplace(syntheticId, std::move(claimed));
    }
    raw->reader = std::thread([this, raw] { readLoop(raw); });
    return ClaimResult::success(syntheticId, ds5Firmware);
}

// True when the packet was a dongle event rather than input, and the loop should take the next
// report. Connect and disconnect interleave with input on the Steam dongle.
bool HidrawGateway::handleSteamWirelessEvent(Claimed* c, const std::uint8_t* data,
                                             std::size_t len) {
    switch (input::usbparse::checkWirelessEvent(c->parser, data, len)) {
    case input::usbparse::WirelessEvent::Connect:
        controllerArrived(c);
        return true;
    case input::usbparse::WirelessEvent::Disconnect:
        controllerLeft(c);
        return true;
    case input::usbparse::WirelessEvent::None:
        return false;
    }
    return false;
}

// A returning pad has rebooted, so its settings are gone: the manager has the slot's output
// worker put them back, off this thread, which only says the pad is there.
void HidrawGateway::controllerArrived(Claimed* c) {
    c->linked.store(true, std::memory_order_release);
    if (c->onReport) { c->onReport(padLinkReport(PadLinkEvent::ControllerArrived)); }
}

// What a controller's leaving means for its last input, its readings and its settings is the
// manager's (UsbGamepadManager's publisher, and the slot's output worker).
void HidrawGateway::controllerLeft(Claimed* c) {
    c->linked.store(false, std::memory_order_release);
    if (c->onReport) { c->onReport(padLinkReport(PadLinkEvent::ControllerLeft)); }
}

// A descriptor-driven layout wins where the device published one; everything else goes through the
// per-model decoder chosen at claim time.
bool HidrawGateway::decodeOneReport(Claimed* c, const std::uint8_t* data, std::size_t len,
                                    input::usbparse::ParsedReport& parsed) {
    if (c->layout.valid) { return input::usbhid::decodeFromLayout(data, len, parsed, c->layout); }
    return input::usbparse::decodeReport(c->parser, data, len, parsed, c->sticks, &c->micMute);
}

// Plain C++ on its own thread, no Qt and no allocation per report: the read buffer and the
// ParsedReport scratch live on this stack and are reused every iteration, and the decoders are
// pure functions over them that mutate the device's stick auto-range state in place.
void HidrawGateway::readLoop(Claimed* c) {
    const thread::ScopedThreadRole role(thread::ThreadRole::UsbReader);
    if (priority_ != nullptr) { priority_->request(thread::ThreadRole::UsbReader); }
    std::array<std::uint8_t, 128> buf{};
    pollfd pfd{};
    pfd.fd = c->fd;
    pfd.events = POLLIN;

    while (c->running.load()) {
        std::size_t len = 0;
        const ReadOutcome outcome = readOneReport(pfd, buf, len);
        if (outcome == ReadOutcome::Stop) { break; }
        if (outcome == ReadOutcome::Idle) { continue; }
        c->completions.fetch_add(1);

        const std::uint8_t* data = buf.data();
        if (input::usbparse::parserHasWirelessEvents(c->parser) &&
            handleSteamWirelessEvent(c, data, len)) {
            continue;
        }

        // A report that does not match the family's shape (wrong id, too short) is skipped rather
        // than published as noise.
        input::usbparse::ParsedReport parsed{};
        if (!decodeOneReport(c, data, len, parsed)) { continue; }
        c->decoded.fetch_add(1);
        // A report means a controller is there, its connect event missed or not.
        if (!c->linked.load(std::memory_order_relaxed)) { controllerArrived(c); }
        if (c->onReport) {
            c->onReport(toUsbReport(parsed, c->micMute.muted.load(std::memory_order_relaxed)));
        }
    }
    c->alive.store(false);
}

void HidrawGateway::releaseClaim(int syntheticId) {
    std::unique_ptr<Claimed> claimed;
    {
        std::lock_guard<thread::TrackedMutex> lock(mtx_);
        const auto it = claimed_.find(syntheticId);
        if (it == claimed_.end()) { return; }
        claimed = std::move(it->second);
        claimed_.erase(it);
    }
    claimed->running.store(false);
    if (claimed->reader.joinable()) { claimed->reader.join(); }
    // The settings the pad held of ours came off it when the manager stopped the claim's output
    // worker, before this; nothing is written to the pad here.
    if (claimed->fd >= 0) {
        ::close(claimed->fd);
        claimed->fd = -1;
    }
}

bool HidrawGateway::isClaimAlive(int syntheticId) const {
    std::lock_guard<thread::TrackedMutex> lock(mtx_);
    const auto it = claimed_.find(syntheticId);
    if (it == claimed_.end()) { return false; }
    return it->second->alive.load();
}

bool HidrawGateway::isPadLinked(int syntheticId) const {
    std::lock_guard<thread::TrackedMutex> lock(mtx_);
    const auto it = claimed_.find(syntheticId);
    if (it == claimed_.end()) { return false; }
    return it->second->linked.load(std::memory_order_acquire);
}

std::int64_t HidrawGateway::completionCount(int syntheticId) const {
    std::lock_guard<thread::TrackedMutex> lock(mtx_);
    const auto it = claimed_.find(syntheticId);
    if (it == claimed_.end()) { return 0; }
    return it->second->completions.load();
}

std::int64_t HidrawGateway::decodedCount(int syntheticId) const {
    std::lock_guard<thread::TrackedMutex> lock(mtx_);
    const auto it = claimed_.find(syntheticId);
    if (it == claimed_.end()) { return 0; }
    return it->second->decoded.load();
}

bool HidrawGateway::setPadMicMuted(int syntheticId, bool muted) {
    std::lock_guard<thread::TrackedMutex> lock(mtx_);
    const auto it = claimed_.find(syntheticId);
    if (it == claimed_.end()) { return false; }
    if (it->second->parser != input::usbparse::HidParser::DualSense) { return false; }
    // The latch alone, never `held`: a UI toggle is not a button press, and
    // touching the edge tracker could swallow or double a real press racing in.
    it->second->micMute.muted.store(muted, std::memory_order_relaxed);
    return true;
}

HidrawGateway::Claimed* HidrawGateway::claimedFor(int syntheticId) const {
    // The claim map's lock is released before any write: a write can block on a sleeping pad, and
    // holding mtx_ across it would stall reconcile(). Safe because releaseClaim() joins the reader
    // and erases the entry only from the owner thread, after the manager stopped the claim's
    // output worker.
    std::lock_guard<thread::TrackedMutex> lock(mtx_);
    const auto it = claimed_.find(syntheticId);
    return it == claimed_.end() ? nullptr : it->second.get();
}

// The claim's own node and write lock; the manager stops the worker that holds the writer before
// it releases the claim, which is what closes the node.
std::unique_ptr<HidReportWriter> HidrawGateway::openReportWriter(int syntheticId) {
    Claimed* c = claimedFor(syntheticId);
    if (c == nullptr || c->fd < 0) { return UsbDeviceGateway::openReportWriter(syntheticId); }
    return std::make_unique<FdReportWriter>(c->fd, &c->writeMtx);
}

bool HidrawGateway::writeOutputReport(int syntheticId, const std::uint8_t* data, std::size_t len) {
    Claimed* c = claimedFor(syntheticId);
    if (c == nullptr) { return false; }
    return FdReportWriter(c->fd, &c->writeMtx).write(data, len);
}

} // namespace dish::source::usb
