// shotref-cam: tiny command-line helper between the shotref bridge and Sony's Camera Remote SDK.
//
//   shotref-cam status          -> {"connected": true, "model": "ILCE-7CM2", "dial": "M"}
//   shotref-cam apply < preset  -> {"results": [{"setting","ok","message"}], "dial": "M"}
//
// STATUS: SKELETON, NOT YET COMPILED. Sony's SDK is not redistributable, so this could not be built
// or tested here. Function and property names follow the SDK's RemoteCli sample, but check every
// call marked "VERIFY" against the API reference of the SDK version you download, especially the
// value encodings (they differ between properties and sometimes between camera models).

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include "CameraRemote_SDK.h"   // from Sony's SDK: app/CRSDK/

using json = nlohmann::json;
namespace SDK = SCRSDK;

// ---------------------------------------------------------------- connection

class Callback : public SDK::IDeviceCallback {
public:
    std::mutex m;
    std::condition_variable cv;
    bool connected = false, failed = false;

    void OnConnected(SDK::DeviceConnectionVersioin) override { std::lock_guard<std::mutex> l(m); connected = true; cv.notify_all(); }
    void OnError(CrInt32u) override { std::lock_guard<std::mutex> l(m); failed = true; cv.notify_all(); }
    void OnDisconnected(CrInt32u) override {}
    void OnPropertyChanged() override {}
    void OnLvPropertyChanged() override {}
    void OnCompleteDownload(CrChar*, CrInt32u) override {}
    void OnWarning(CrInt32u) override {}

    bool wait(int seconds) {
        std::unique_lock<std::mutex> l(m);
        cv.wait_for(l, std::chrono::seconds(seconds), [&] { return connected || failed; });
        return connected;
    }
};

struct Session {
    SDK::ICrEnumCameraObjectInfo* list = nullptr;
    SDK::CrDeviceHandle handle = 0;
    Callback cb;
    std::string model;

    bool open() {
        if (!SDK::Init()) return false;
        // Finds cameras on USB and on the network (camera: MENU → Network → PC Remote Function → On).
        if (CR_FAILED(SDK::EnumCameraObjects(&list)) || !list || list->GetCount() == 0) return false;
        auto* info = list->GetCameraObjectInfo(0);
        model = info->GetModel();
        // VERIFY: Connect() gained extra parameters (control mode, reconnect, user/password for
        // Access Authentication) in newer SDK versions. Copy the exact call from RemoteCli.
        if (CR_FAILED(SDK::Connect(const_cast<SDK::ICrCameraObjectInfo*>(info), &cb, &handle))) return false;
        return cb.wait(10);
    }
    ~Session() {
        if (handle) SDK::Disconnect(handle);
        if (list) list->Release();
        SDK::Release();
    }
};

// ---------------------------------------------------------------- properties

static bool setProp(SDK::CrDeviceHandle h, SDK::CrDevicePropertyCode code, std::uint64_t value, SDK::CrDataType type) {
    SDK::CrDeviceProperty p;
    p.SetCode(code);
    p.SetCurrentValue(value);
    p.SetValueType(type);
    return CR_SUCCEEDED(SDK::SetDeviceProperty(h, &p));
}

// Reads the list of values the camera currently accepts for a property (e.g. all apertures of the lens).
static std::vector<std::uint64_t> possibleValues(SDK::CrDeviceHandle h, SDK::CrDevicePropertyCode code) {
    std::vector<std::uint64_t> out;
    SDK::CrDeviceProperty* props = nullptr;
    CrInt32 n = 0;
    CrInt32u codes[] = {static_cast<CrInt32u>(code)};
    if (CR_FAILED(SDK::GetSelectDeviceProperties(h, 1, codes, &props, &n)) || n < 1) return out;
    // VERIFY: values are packed by data type; this assumes 16-bit values (true for F-number).
    auto* raw = reinterpret_cast<const std::uint16_t*>(props[0].GetValues());
    for (CrInt32u i = 0; i < props[0].GetValueSize() / sizeof(std::uint16_t); ++i) out.push_back(raw[i]);
    SDK::ReleaseDeviceProperties(h, props);
    return out;
}

// VERIFY all encodings below against the API reference.
static std::uint64_t encodeFNumber(double f) { return static_cast<std::uint64_t>(f * 100 + 0.5); }      // f/2.8 -> 280
static std::uint64_t encodeShutter(const std::string& s) {                                              // numerator<<16 | denominator
    if (s == "bulb") return 0;
    if (s.rfind("1/", 0) == 0) return (1ull << 16) | std::stoul(s.substr(2));
    double sec = std::stod(s);                                                                         // e.g. 15" or 2.5"
    return (static_cast<std::uint64_t>(sec * 10 + 0.5) << 16) | 10;
}
static std::uint64_t encodeIso(const json& v) { return v.is_string() ? 0xFFFFFF : v.get<std::uint64_t>(); }  // 0xFFFFFF = AUTO

static json result(const std::string& setting, bool ok, const std::string& msg) {
    return {{"setting", setting}, {"ok", ok}, {"message", msg}};
}

static json apply(Session& s, const json& p) {
    json results = json::array();
    auto h = s.handle;

    // Aperture ('widest' = smallest F-number the lens offers right now)
    {
        std::uint64_t v;
        if (p["fnumber"] == "widest") {
            auto vals = possibleValues(h, SDK::CrDevicePropertyCode::CrDeviceProperty_FNumber);
            v = vals.empty() ? 0 : *std::min_element(vals.begin(), vals.end());
        } else {
            v = encodeFNumber(p["fnumber"].get<double>());
        }
        bool ok = v && setProp(h, SDK::CrDevicePropertyCode::CrDeviceProperty_FNumber, v, SDK::CrDataType::CrDataType_UInt16);
        results.push_back(result("fnumber", ok, ok ? "f/" + std::to_string(v / 100.0).substr(0, 3) : "not accepted"));
    }
    if (!p["shutter"].is_null()) {
        bool ok = setProp(h, SDK::CrDevicePropertyCode::CrDeviceProperty_ShutterSpeed, encodeShutter(p["shutter"]), SDK::CrDataType::CrDataType_UInt32);
        results.push_back(result("shutter", ok, ok ? p["shutter"].get<std::string>() : "not accepted (check mode dial)"));
    }
    {
        bool ok = setProp(h, SDK::CrDevicePropertyCode::CrDeviceProperty_IsoSensitivity, encodeIso(p["iso"]), SDK::CrDataType::CrDataType_UInt32);
        results.push_back(result("iso", ok, ok ? "ISO " + p["iso"].dump() : "not accepted"));
    }
    {
        // TODO: map preset names (auto/daylight/...) to CrWhiteBalanceSetting values,
        // and for Kelvin values set WB to Color Temp first, then CrDeviceProperty_Colortemp.
        results.push_back(result("wb", false, "not implemented yet"));
    }
    {
        // TODO: CrDeviceProperty_FocusMode with CrFocusMode values (AF_S / AF_C / MF).
        results.push_back(result("focus", false, "not implemented yet"));
    }
    {
        // TODO: CrDeviceProperty_DriveMode with CrDriveMode values (single, continuous lo/hi+, self-timer 2 s).
        results.push_back(result("drive", false, "not implemented yet"));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let the camera settle before disconnecting
    return {{"results", results}, {"dial", "unknown"}};  // TODO: read CrDeviceProperty_ExposureProgramMode
}

int main(int argc, char** argv) {
    std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd != "status" && cmd != "apply") {
        std::cerr << "usage: shotref-cam status | apply < preset.json\n";
        return 2;
    }
    Session s;
    if (!s.open()) {
        if (cmd == "status") { std::cout << json{{"connected", false}}.dump() << "\n"; return 0; }
        std::cerr << "no camera connected\n";
        return 1;
    }
    if (cmd == "status") {
        std::cout << json{{"connected", true}, {"model", s.model}, {"dial", "unknown"}}.dump() << "\n";
        return 0;
    }
    json preset;
    try { std::cin >> preset; } catch (...) { std::cerr << "invalid preset JSON\n"; return 2; }
    std::cout << apply(s, preset).dump() << "\n";
    return 0;
}
