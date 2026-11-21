// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A pad's hardware from its own descriptors, the capture of its reports at rest, and the
// measured usbHost (docs/diagnostics-bundle.md): the pure core on real bytes, and the Linux sysfs
// readers on a fake tree.

#include "core/diagnostics/Json.h"
#include "core/diagnostics/ReportCapture.h"
#include "core/diagnostics/UsbDescriptors.h"
#include "core/diagnostics/UsbHost.h"
#include "source/usb/UsbSysfs.h"

#include "UsbDescriptorFixtures.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace dish::diagnostics;
using dish::test::kDualSenseConfiguration;
using dish::test::kDualSenseDevice;
using dish::test::kDualSenseReportDescriptor;
using dish::test::kDualSenseReportDescriptorSha256;
using dish::test::kRootHub2Descriptors;
using dish::test::kRootHub3Descriptors;

namespace {

template <std::size_t N> DescriptorBytes bytesOf(const std::array<std::uint8_t, N>& a) {
    return DescriptorBytes(a.begin(), a.end());
}

UsbEndpoint endpointOf(std::uint8_t address, EndpointType type, std::uint16_t maxPacket,
                       std::uint8_t interval) {
    return UsbEndpoint{address, type, maxPacket, interval};
}

UsbInterface interfaceOf(std::uint8_t number, std::uint8_t alternate, std::uint8_t cls,
                         std::uint8_t subclass, std::vector<UsbEndpoint> endpoints = {}) {
    return UsbInterface{number, alternate, cls, subclass, 0x00, std::move(endpoints)};
}

UsbHardwareFacts dualSenseFacts() {
    UsbHardwareFacts facts;
    facts.bcdDevice = 0x0100;
    facts.raw = RawDescriptors{bytesOf(kDualSenseDevice), bytesOf(kDualSenseConfiguration)};
    facts.interfaces = interfacesOf(facts.raw.configuration);
    facts.strings = {{1, "Sony Interactive Entertainment"}, {2, "Wireless Controller"}};
    facts.reportDescriptor =
        HidReportDescriptor{3, bytesOf(kDualSenseReportDescriptor), DescriptorSource::Device};
    return facts;
}

// One file under `dir`, its folders made on the way.
void writeFile(const QString& dir, const QString& name, const QByteArray& body) {
    const QString path = dir + QLatin1Char('/') + name;
    REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(body);
}

template <std::size_t N> QByteArray qbytesOf(const std::array<std::uint8_t, N>& a) {
    return QByteArray(reinterpret_cast<const char*>(a.data()), static_cast<qsizetype>(N));
}

} // namespace

TEST_CASE("usb descriptors: the device descriptor names the release and the strings' indices",
          "[diagnostics][usb]") {
    const auto ds5 = parseDeviceDescriptor(bytesOf(kDualSenseDevice));
    REQUIRE(ds5.has_value());
    CHECK(ds5->bcdUsb == 0x0200);
    CHECK(ds5->vendorId == 0x054C);
    CHECK(ds5->productId == 0x0CE6);
    CHECK(ds5->bcdDevice == 0x0100);
    CHECK(ds5->manufacturerIndex == 1);
    CHECK(ds5->productIndex == 2);
    CHECK(ds5->serialNumberIndex == 0);
    CHECK(ds5->configurations == 1);

    const RawDescriptors hub =
        splitDescriptors(kRootHub2Descriptors.data(), kRootHub2Descriptors.size());
    const auto hubDevice = parseDeviceDescriptor(hub.device);
    REQUIRE(hubDevice.has_value());
    CHECK(hubDevice->bcdDevice == 0x0700);
    CHECK(hubDevice->serialNumberIndex == 1);

    SECTION("anything shorter or of another type is no device descriptor") {
        CHECK_FALSE(parseDeviceDescriptor(DescriptorBytes{0x12, 0x01}).has_value());
        DescriptorBytes config = bytesOf(kDualSenseDevice);
        config[1] = 0x02;
        CHECK_FALSE(parseDeviceDescriptor(config).has_value());
    }
}

TEST_CASE("usb descriptors: the file splits at the device descriptor's own length",
          "[diagnostics][usb]") {
    const RawDescriptors hub =
        splitDescriptors(kRootHub2Descriptors.data(), kRootHub2Descriptors.size());
    CHECK(hub.device.size() == 18);
    CHECK(hub.configuration.size() == 25);
    CHECK(hub.configuration[1] == 0x02);
    CHECK(splitDescriptors(nullptr, 0) == RawDescriptors{});
    // A file cut short keeps what is there as the device descriptor.
    const RawDescriptors cut = splitDescriptors(kRootHub2Descriptors.data(), 10);
    CHECK(cut.device.size() == 10);
    CHECK(cut.configuration.empty());
}

TEST_CASE("usb descriptors: every interface and alternate setting with the endpoints after it",
          "[diagnostics][usb]") {
    const std::vector<UsbInterface> interfaces = interfacesOf(bytesOf(kDualSenseConfiguration));
    const std::vector<UsbInterface> expected{
        interfaceOf(0, 0, 0x01, 0x01),
        interfaceOf(1, 0, 0x01, 0x02),
        interfaceOf(1, 1, 0x01, 0x02, {endpointOf(0x01, EndpointType::Isochronous, 392, 4)}),
        interfaceOf(2, 0, 0x01, 0x02),
        interfaceOf(2, 1, 0x01, 0x02, {endpointOf(0x82, EndpointType::Isochronous, 196, 4)}),
        interfaceOf(3, 0, 0x03, 0x00,
                    {endpointOf(0x84, EndpointType::Interrupt, 64, 6),
                     endpointOf(0x03, EndpointType::Interrupt, 64, 6)}),
    };
    CHECK(interfaces == expected);
    CHECK(interfaces.back().endpoints.front().isIn());
    CHECK_FALSE(interfaces.back().endpoints.back().isIn());

    SECTION("a SuperSpeed companion descriptor is stepped over") {
        const RawDescriptors hub =
            splitDescriptors(kRootHub3Descriptors.data(), kRootHub3Descriptors.size());
        const std::vector<UsbInterface> hubInterfaces = interfacesOf(hub.configuration);
        REQUIRE(hubInterfaces.size() == 1);
        CHECK(hubInterfaces[0].interfaceClass == 0x09);
        CHECK(hubInterfaces[0].endpoints ==
              std::vector<UsbEndpoint>{endpointOf(0x81, EndpointType::Interrupt, 2, 12)});
    }
}

TEST_CASE("usb descriptors: a descriptor that lies about its length ends the walk",
          "[diagnostics][usb]") {
    // interface 0, then a zero-length descriptor that would never advance.
    const DescriptorBytes zeroLength{0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00,
                                     0x00, 0x00, 0x05, 0x81, 0x03, 0x40, 0x00, 0x01};
    CHECK(interfacesOf(zeroLength).size() == 1);
    CHECK(interfacesOf(zeroLength).front().endpoints.empty());
    // an endpoint that claims more bytes than are left.
    const DescriptorBytes overrun{0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x00,
                                  0x00, 0x00, 0x09, 0x05, 0x81, 0x03};
    CHECK(interfacesOf(overrun).front().endpoints.empty());
    // an endpoint before any interface, and an interface descriptor too short to be one.
    const DescriptorBytes orphan{0x07, 0x05, 0x81, 0x03, 0x40, 0x00,
                                 0x01, 0x05, 0x04, 0x00, 0x00, 0x01};
    CHECK(interfacesOf(orphan).empty());
    CHECK(interfacesOf(DescriptorBytes{}).empty());
}

TEST_CASE("usb descriptors: the report descriptor is named by its SHA-256", "[diagnostics][usb]") {
    CHECK(sha256Hex(bytesOf(kDualSenseReportDescriptor)) == kDualSenseReportDescriptorSha256);
    CHECK(sha256Hex(DescriptorBytes{}).empty());
    UsbHardwareFacts none;
    CHECK(descriptorSha256Json(none).kind() == Json::Kind::Null);
    CHECK(descriptorSha256Json(dualSenseFacts()).stringValue() == kDualSenseReportDescriptorSha256);
}

TEST_CASE("usb descriptors: release numbers and bytes read as the bundle writes them",
          "[diagnostics][usb]") {
    CHECK(bcdText(0x0107) == "0107");
    CHECK(bcdText(0x0100) == "0100");
    CHECK(hexOf(DescriptorBytes{0x12, 0x01, 0xAB}) == "1201ab");
    CHECK(endpointTypeToken(EndpointType::Interrupt) == "interrupt");
    CHECK(endpointTypeToken(EndpointType::Isochronous) == "isochronous");
    CHECK(endpointTypeToken(EndpointType::Bulk) == "bulk");
    CHECK(endpointTypeToken(EndpointType::Control) == "control");
}

TEST_CASE("usb descriptors: a capture's strings are the maker's and the product's, never the "
          "serial number's",
          "[diagnostics][usb]") {
    UsbDeviceDescriptor device;
    device.manufacturerIndex = 1;
    device.productIndex = 2;
    device.serialNumberIndex = 3;
    CHECK(
        captureStrings(device, "Sony Interactive Entertainment", "Wireless Controller") ==
        std::vector<UsbString>{{1, "Sony Interactive Entertainment"}, {2, "Wireless Controller"}});
    SECTION("a string that shares the serial number's index is the serial number") {
        device.productIndex = 3;
        CHECK(captureStrings(device, "Maker", "Unit 0042") == std::vector<UsbString>{{1, "Maker"}});
    }
    SECTION("index 0 names no string, and an empty one is not written") {
        device.manufacturerIndex = 0;
        CHECK(captureStrings(device, "Maker", "").empty());
    }
}

TEST_CASE("usb descriptors: each interface is written with its endpoints", "[diagnostics][usb]") {
    const Json interfaces = interfacesJson(dualSenseFacts().interfaces);
    REQUIRE(interfaces.items().size() == 6);
    const Json& hid = interfaces.items().back();
    CHECK(hid.find("number")->integerValue() == 3);
    CHECK(hid.find("alternate")->integerValue() == 0);
    CHECK(hid.find("class")->stringValue() == "03");
    CHECK(hid.find("subclass")->stringValue() == "00");
    CHECK(hid.find("protocol")->stringValue() == "00");
    const Json& in = hid.find("endpoints")->items().front();
    CHECK(in.find("address")->stringValue() == "84");
    CHECK(in.find("type")->stringValue() == "interrupt");
    CHECK(in.find("maxPacketSize")->integerValue() == 64);
    CHECK(in.find("interval")->integerValue() == 6);
}

TEST_CASE("capture: twenty reports at rest, then the capture disarms itself",
          "[diagnostics][capture]") {
    ReadLoopWord word;
    word.start();
    ReportCapture capture;
    CHECK_FALSE(ReadLoopWord::capturing(word.load()));
    REQUIRE(capture.arm(word));
    CHECK(ReadLoopWord::capturing(word.load()));
    CHECK(ReadLoopWord::running(word.load()));
    CHECK_FALSE(capture.arm(word));
    for (std::uint8_t i = 0; i < 25; ++i) {
        const std::array<std::uint8_t, 3> report{0x01, i, 0x80};
        if (ReadLoopWord::capturing(word.load())) {
            capture.offer(word, report.data(), report.size(), 1000 + i);
        }
    }
    const std::vector<CapturedReport> kept = capture.kept();
    REQUIRE(kept.size() == kCaptureReports);
    CHECK(kept.front().bytes[1] == 0);
    CHECK(kept.back().bytes[1] == 19);
    CHECK(kept.back().atMs == 1019);
    CHECK(kept.back().length == 3);
    CHECK_FALSE(ReadLoopWord::capturing(word.load()));
    CHECK(ReadLoopWord::running(word.load()));

    SECTION("a capture armed again starts over at the first slot") {
        REQUIRE(capture.arm(word));
        CHECK(capture.kept().empty());
        const std::array<std::uint8_t, 1> report{0x42};
        capture.offer(word, report.data(), report.size(), 5000);
        REQUIRE(capture.kept().size() == 1);
        CHECK(capture.kept().front().bytes[0] == 0x42);
    }
}

TEST_CASE("capture: a report longer than a slot keeps its first bytes", "[diagnostics][capture]") {
    ReadLoopWord word;
    ReportCapture capture;
    REQUIRE(capture.arm(word));
    std::array<std::uint8_t, 100> report{};
    report[63] = 0x63;
    report[64] = 0x64;
    capture.offer(word, report.data(), report.size(), 7);
    REQUIRE(capture.kept().size() == 1);
    CHECK(capture.kept().front().length == kCaptureReportMaxBytes);
    CHECK(capture.kept().front().bytes[63] == 0x63);
}

TEST_CASE("capture: the loop's word keeps running and capturing apart", "[diagnostics][capture]") {
    ReadLoopWord word;
    CHECK_FALSE(ReadLoopWord::running(word.load()));
    word.start();
    REQUIRE(word.armCapture());
    word.stop();
    CHECK_FALSE(ReadLoopWord::running(word.load()));
    CHECK(ReadLoopWord::capturing(word.load()));
    word.disarmCapture();
    CHECK_FALSE(ReadLoopWord::capturing(word.load()));
    SECTION("an unarmed capture keeps nothing it is offered") {
        ReportCapture capture;
        const std::array<std::uint8_t, 1> report{0x01};
        capture.offer(word, report.data(), report.size(), 1);
        CHECK(capture.kept().empty());
    }
}

TEST_CASE("capture: done at twenty reports, or when its time is up, failed without the pad",
          "[diagnostics][capture]") {
    CHECK(captureProgress(kCaptureReports, 3, 20, true) == CaptureProgress::Captured);
    CHECK(captureProgress(5, 3, 20, true) == CaptureProgress::Capturing);
    // A pad that reports only on change sends nothing at rest: its descriptors still ride.
    CHECK(captureProgress(0, 20, 20, true) == CaptureProgress::Captured);
    CHECK(captureProgress(kCaptureReports, 3, 20, false) == CaptureProgress::Failed);
}

TEST_CASE("capture: arrival times move onto the wall clock by their age",
          "[diagnostics][capture]") {
    std::vector<CapturedReport> kept(2);
    kept[0].atMs = 100;
    kept[0].length = 2;
    kept[0].bytes[0] = 0x01;
    kept[0].bytes[1] = 0x7F;
    kept[1].atMs = 104;
    kept[1].length = 1;
    const std::vector<RestReport> reports = restReportsOf(kept, 110, 1763586000000);
    REQUIRE(reports.size() == 2);
    CHECK(reports[0] == RestReport{1763585999990, DescriptorBytes{0x01, 0x7F}});
    CHECK(reports[1].atMs == 1763585999994);
    CHECK(reports[1].bytes.size() == 1);
}

TEST_CASE("capture: the file's usb-descriptors-v1 object", "[diagnostics][capture]") {
    const std::vector<RestReport> reports{RestReport{1763585999990, DescriptorBytes{0x01, 0x80}}};
    const Json capture = captureJson(dualSenseFacts(), reports);
    CHECK(capture.find("format")->stringValue() == "usb-descriptors-v1");
    CHECK(capture.find("device")->stringValue() == "12010002000000404c05e60c0001010200"
                                                   "01");
    CHECK(capture.find("configuration")->stringValue().size() ==
          2 * kDualSenseConfiguration.size());
    CHECK(capture.find("strings")->find("1")->stringValue() == "Sony Interactive Entertainment");
    CHECK(capture.find("strings")->find("2")->stringValue() == "Wireless Controller");
    const Json& descriptor = capture.find("hidReportDescriptors")->items().front();
    CHECK(descriptor.find("interface")->integerValue() == 3);
    CHECK(descriptor.find("source")->stringValue() == "device");
    CHECK(descriptor.find("hex")->stringValue().rfind("05010905a101", 0) == 0);
    const Json& rest = capture.find("restReports")->items().front();
    CHECK(rest.find("atMs")->integerValue() == 1763585999990);
    CHECK(rest.find("hex")->stringValue() == "0180");

    SECTION("what the platform could not read is null, or empty") {
        const Json none = captureJson(UsbHardwareFacts{}, {});
        CHECK(none.find("device")->kind() == Json::Kind::Null);
        CHECK(none.find("configuration")->kind() == Json::Kind::Null);
        CHECK(none.find("hidReportDescriptors")->items().empty());
        CHECK(none.find("restReports")->items().empty());
    }
}

TEST_CASE("usb host: a host controller is true, none is false, no answer is null",
          "[diagnostics][usb]") {
    CHECK(usbHostOf(2) == std::optional<bool>(true));
    CHECK(usbHostOf(1) == std::optional<bool>(true));
    CHECK(usbHostOf(0) == std::optional<bool>(false));
    CHECK_FALSE(usbHostOf(std::nullopt).has_value());
}

TEST_CASE("usb host: Linux counts the root hubs usbN, not the devices or their interfaces",
          "[diagnostics][usb][sysfs]") {
    QTemporaryDir root;
    REQUIRE(root.isValid());
    const QString devices = root.filePath(QStringLiteral("bus/usb/devices"));
    for (const char* name : {"usb1", "usb2", "1-0:1.0", "2-0:1.0", "1-2", "1-2:1.3", "usbmon"}) {
        REQUIRE(QDir().mkpath(devices + QLatin1Char('/') + QLatin1String(name)));
    }
    CHECK(dish::source::usb::countRootHubs(devices.toStdString()) == std::optional<int>(2));

    SECTION("a bus with no root hub is none") {
        const QString bare = root.filePath(QStringLiteral("bare"));
        REQUIRE(QDir().mkpath(bare + QStringLiteral("/1-0:1.0")));
        CHECK(dish::source::usb::countRootHubs(bare.toStdString()) == std::optional<int>(0));
    }
    SECTION("no such directory is no answer") {
        CHECK_FALSE(
            dish::source::usb::countRootHubs(root.filePath(QStringLiteral("none")).toStdString())
                .has_value());
    }
}

TEST_CASE("usb hardware: a pad's facts come from its sysfs device, never its serial number",
          "[diagnostics][usb][sysfs]") {
    QTemporaryDir root;
    REQUIRE(root.isValid());
    const QString device = root.filePath(QStringLiteral("devices/usb1/1-2"));
    QByteArray descriptors = qbytesOf(kDualSenseDevice);
    descriptors += qbytesOf(kDualSenseConfiguration);
    writeFile(device, QStringLiteral("descriptors"), descriptors);
    writeFile(device, QStringLiteral("bcdDevice"), "0100\n");
    writeFile(device, QStringLiteral("manufacturer"), "Sony Interactive Entertainment\n");
    writeFile(device, QStringLiteral("product"), "Wireless Controller\n");
    writeFile(device, QStringLiteral("serial"), "SERIAL-0042\n");
    const DescriptorBytes reportDescriptor = bytesOf(kDualSenseReportDescriptor);

    const UsbHardwareFacts facts =
        dish::source::usb::readUsbHardware(device.toStdString(), 3, reportDescriptor);
    CHECK(facts.bcdDevice == std::optional<std::uint16_t>(0x0100));
    CHECK(facts.raw.device == bytesOf(kDualSenseDevice));
    CHECK(facts.raw.configuration == bytesOf(kDualSenseConfiguration));
    CHECK(facts.interfaces.size() == 6);
    CHECK(facts.strings == std::vector<UsbString>{{1, "Sony Interactive Entertainment"},
                                                  {2, "Wireless Controller"}});
    REQUIRE(facts.reportDescriptor.has_value());
    CHECK(facts.reportDescriptor->interfaceNumber == 3);
    CHECK(facts.reportDescriptor->source == DescriptorSource::Device);
    CHECK(facts.reportDescriptor->bytes == reportDescriptor);
    const std::string written =
        captureJson(facts, {}).dump() + interfacesJson(facts.interfaces).dump();
    CHECK(written.find("SERIAL-0042") == std::string::npos);

    SECTION("a device whose files cannot be read has none of them") {
        const UsbHardwareFacts none = dish::source::usb::readUsbHardware(
            root.filePath(QStringLiteral("devices/none")).toStdString(), 0, {});
        CHECK(none == UsbHardwareFacts{});
    }
}
