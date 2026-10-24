// shotref-cam: small helper between the shotref bridge and Sony's Camera Remote SDK.
//
// Written against Camera Remote SDK v2.02.00 (headers, RemoteCli sample and API reference).
//
//   shotref-cam serve            long-running: one JSON request per line on stdin, one JSON answer per line
//                                on stdout. Keeps the camera session open (needed: PC-remote priority is
//                                dropped by the camera when the session closes).
//   shotref-cam status           one-shot status, for testing
//   shotref-cam apply < preset   one-shot apply, for testing
//
// Requests in serve mode:
//   {"cmd":"status"}                  -> {"connected":true,"model":"ILCE-7CM2","dial":"M"}
//   {"cmd":"apply","preset":{...}}    -> {"results":[{"setting","ok","message"}],"dial":"M"}
//   {"cmd":"release"}                 -> {"ok":true}   hand the dials back to the camera
//
// SSH (Wi-Fi access point connections): user/password from SHOTREF_CAM_USER / SHOTREF_CAM_PASSWORD.
// The camera's fingerprint is trusted on first use and stored in SHOTREF_CAM_FINGERPRINT_FILE
// (default ~/.shotref-camera-fingerprint); a changed fingerprint is refused.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include "CameraRemote_SDK.h"
#include "IDeviceCallback.h"

using json = nlohmann::json;
namespace SDK = SCRSDK;
using namespace std::chrono_literals;
using Code = SDK::CrDevicePropertyCode;

// ---------------------------------------------------------------- helpers

static std::string toUtf8(const CrChar* s) {
    if (!s) return {};
#if defined(_WIN32)
    std::string out;
    for (; *s; ++s) out += (*s < 0x80) ? static_cast<char>(*s) : '?';
    return out;
#else
    return std::string(s);
#endif
}

static std::string envOr(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

static std::string fingerprintFile() {
    std::string home = envOr("HOME", envOr("USERPROFILE", "."));
    return envOr("SHOTREF_CAM_FINGERPRINT_FILE", home + "/.shotref-camera-fingerprint");
}

static json result(const std::string& setting, bool ok, const std::string& msg) {
    return {{"setting", setting}, {"ok", ok}, {"message", msg}};
}

// A property as read from the camera: current value, writability and the values it accepts right now.
struct Prop {
    bool found = false;
    bool writable = false;
    SDK::CrDataType type = SDK::CrDataType_Undefined;
    std::uint64_t current = 0;
    std::vector<std::uint64_t> values;  // for *Array types: allowed values; for *Range: {min, max, step}
};

static std::size_t elemSize(SDK::CrDataType t) {
    switch (t & 0x0FFF) {
        case SDK::CrDataType_UInt8:  return 1;
        case SDK::CrDataType_UInt16: return 2;
        case SDK::CrDataType_UInt32: return 4;
        case SDK::CrDataType_UInt64: return 8;
        default: return 0;
    }
}

// ---------------------------------------------------------------- camera session

class Callback : public SDK::IDeviceCallback {
public:
    std::mutex m;
    std::condition_variable cv;
    bool connected = false;
    CrInt32u error = 0;

    void OnConnected(SDK::DeviceConnectionVersioin) override {
        std::lock_guard<std::mutex> l(m); connected = true; error = 0; cv.notify_all();
    }
    void OnDisconnected(CrInt32u) override { std::lock_guard<std::mutex> l(m); connected = false; }
    void OnError(CrInt32u e) override { std::lock_guard<std::mutex> l(m); error = e ? e : 1; cv.notify_all(); }

    bool waitConnected(std::chrono::seconds timeout) {
        std::unique_lock<std::mutex> l(m);
        cv.wait_for(l, timeout, [&] { return connected || error; });
        return connected;
    }
    bool isConnected() { std::lock_guard<std::mutex> l(m); return connected; }
};

class Camera {
public:
    std::string model;
    std::string lastError;

    ~Camera() { close(); }

    bool ensureOpen() {
        if (handle_ && cb_.isConnected()) return true;
        close();
        return open();
    }

    void close() {
        if (handle_) {
            SDK::Disconnect(handle_);
            SDK::ReleaseDevice(handle_);
            handle_ = 0;
        }
        if (list_) { list_->Release(); list_ = nullptr; }
        std::lock_guard<std::mutex> l(cb_.m);
        cb_.connected = false; cb_.error = 0;
    }

    Prop get(Code code) {
        Prop p;
        SDK::CrDeviceProperty* props = nullptr;
        CrInt32 n = 0;
        CrInt32u codes[] = {static_cast<CrInt32u>(code)};
        if (CR_FAILED(SDK::GetSelectDeviceProperties(handle_, 1, codes, &props, &n)) || !props) return p;
        for (CrInt32 i = 0; i < n; ++i) {
            if (props[i].GetCode() != static_cast<CrInt32u>(code)) continue;
            p.found = true;
            p.writable = props[i].IsSetEnableCurrentValue();
            p.type = props[i].GetValueType();
            p.current = props[i].GetCurrentValue();
            std::size_t es = elemSize(p.type);
            const CrInt8u* raw = props[i].GetValues();
            if (es && raw) {
                for (CrInt32u off = 0; off + es <= props[i].GetValueSize(); off += es) {
                    std::uint64_t v = 0;
                    std::memcpy(&v, raw + off, es);  // little endian on all supported platforms
                    p.values.push_back(v);
                }
            }
        }
        SDK::ReleaseDeviceProperties(handle_, props);
        return p;
    }

    bool set(Code code, std::uint64_t value, SDK::CrDataType type) {
        SDK::CrDeviceProperty p;
        p.SetCode(code);
        p.SetCurrentValue(value);
        p.SetValueType(type);
        return CR_SUCCEEDED(SDK::SetDeviceProperty(handle_, &p));
    }

private:
    SDK::ICrEnumCameraObjectInfo* list_ = nullptr;
    SDK::CrDeviceHandle handle_ = 0;
    Callback cb_;

    bool open() {
        lastError.clear();
        if (CR_FAILED(SDK::EnumCameraObjects(&list_, 3)) || !list_ || list_->GetCount() == 0) {
            lastError = "no camera found (USB: PC Remote mode; Wi-Fi: PC Remote Function on and joined the same network)";
            return false;
        }
        auto* info = const_cast<SDK::ICrCameraObjectInfo*>(list_->GetCameraObjectInfo(0));
        model = toUtf8(info->GetModel());

        std::string user, pass, fp;
        if (info->GetSSHsupport() == SDK::CrSSHsupport_ON) {
            user = envOr("SHOTREF_CAM_USER", "admin");
            pass = envOr("SHOTREF_CAM_PASSWORD", "");
            if (pass.empty()) {
                lastError = "this connection needs SSH: set SHOTREF_CAM_PASSWORD (camera: Network → Access Authen. Settings)";
                return false;
            }
            char buf[128] = {0};
            CrInt32u len = 0;
            if (CR_FAILED(SDK::GetFingerprint(info, buf, &len))) { lastError = "could not read the camera's SSH fingerprint"; return false; }
            fp.assign(buf, len);
            std::string known;
            { std::ifstream in(fingerprintFile()); std::getline(in, known); }
            if (known.empty()) {
                std::ofstream(fingerprintFile()) << fp << "\n";  // trust on first use
            } else if (known != fp) {
                lastError = "camera fingerprint changed — refusing to connect. Delete " + fingerprintFile() + " only if you replaced or reset the camera.";
                return false;
            }
        }

        SDK::CrError err = SDK::Connect(info, &cb_, &handle_, SDK::CrSdkControlMode_Remote, SDK::CrReconnecting_ON,
                                   user.empty() ? nullptr : user.c_str(), pass.empty() ? nullptr : pass.c_str(),
                                   fp.empty() ? nullptr : fp.c_str(), static_cast<CrInt32u>(fp.size()));
        if (CR_FAILED(err) || !cb_.waitConnected(15s)) {
            lastError = "camera did not accept the connection";
            close();
            return false;
        }
        std::this_thread::sleep_for(1s);  // let the camera publish its properties
        return true;
    }
};

// ---------------------------------------------------------------- preset mapping

static std::string dialName(std::uint64_t v) {
    switch (v) {
        case SDK::CrExposure_M_Manual: return "M";
        case SDK::CrExposure_A_AperturePriority: return "A";
        case SDK::CrExposure_S_ShutterSpeedPriority: return "S";
        case SDK::CrExposure_P_Auto: return "P";
        default: return "other";
    }
}

static std::optional<double> shutterSeconds(std::uint64_t v) {
    if (v == SDK::CrShutterSpeed_Bulb || v == SDK::CrShutterSpeed_Nothing) return std::nullopt;
    double num = static_cast<double>(v >> 16), den = static_cast<double>(v & 0xFFFF);
    if (den == 0) return std::nullopt;
    return num / den;
}

static std::string shutterLabel(std::uint64_t v) {
    if (v == SDK::CrShutterSpeed_Bulb) return "BULB";
    std::uint64_t num = v >> 16, den = v & 0xFFFF;
    if (num == 1 && den > 1) return "1/" + std::to_string(den) + " s";
    char b[32]; std::snprintf(b, sizeof b, "%g s", static_cast<double>(num) / static_cast<double>(den ? den : 1));
    return b;
}

static double parseShutter(const std::string& s) {
    if (s.rfind("1/", 0) == 0) return 1.0 / std::stod(s.substr(2));
    return std::stod(s);  // 15" -> 15 (stod stops at the quote)
}

// Picks the allowed value closest to the target (on a log scale for apertures/shutter speeds).
template <class Key>
static std::optional<std::uint64_t> closest(const std::vector<std::uint64_t>& vals, double target, Key key) {
    std::optional<std::uint64_t> best;
    double bestD = 1e300;
    for (auto v : vals) {
        auto k = key(v);
        if (!k || *k <= 0) continue;
        double d = std::fabs(std::log(*k) - std::log(target));
        if (d < bestD) { bestD = d; best = v; }
    }
    return best;
}

class Applier {
public:
    explicit Applier(Camera& c) : cam(c) {}

    json apply(const json& p) {
        json results = json::array();

        // 1. Let the remote side win over the physical dials while we are connected.
        takePriority();

        // 2. Exposure mode (M / A) — no need to turn the mode dial.
        {
            std::uint64_t want = p.at("mode") == "M" ? SDK::CrExposure_M_Manual : SDK::CrExposure_A_AperturePriority;
            bool ok = setFromList(Code::CrDeviceProperty_ExposureProgramMode, want);
            results.push_back(result("mode", ok, ok ? p.at("mode").get<std::string>() : "turn the mode dial"));
            std::this_thread::sleep_for(500ms);  // required by the SDK before shutter/aperture changes
        }

        // 3. Aperture: value = F-number * 100. "widest" = smallest the lens offers right now.
        {
            Prop pr = cam.get(Code::CrDeviceProperty_FNumber);
            std::vector<std::uint64_t> usable;
            for (auto v : pr.values) if (v > 0 && v < SDK::CrFnumber_IrisClose) usable.push_back(v);
            std::optional<std::uint64_t> pick;
            if (p.at("fnumber") == "widest") {
                if (!usable.empty()) pick = *std::min_element(usable.begin(), usable.end());
            } else {
                pick = closest(usable, p.at("fnumber").get<double>() * 100, [](std::uint64_t v) { return std::optional<double>(double(v)); });
            }
            bool ok = pick && pr.writable && cam.set(Code::CrDeviceProperty_FNumber, *pick, pr.type);
            char b[16]; if (pick) std::snprintf(b, sizeof b, "f/%g", *pick / 100.0);
            results.push_back(result("fnumber", ok, ok ? b : notSet(pr)));
        }

        // 4. Shutter speed (only in M): pick the camera's own value nearest to the target.
        if (!p.at("shutter").is_null()) {
            Prop pr = cam.get(Code::CrDeviceProperty_ShutterSpeed);
            std::string s = p.at("shutter");
            std::optional<std::uint64_t> pick;
            if (s == "bulb") {
                if (std::find(pr.values.begin(), pr.values.end(), SDK::CrShutterSpeed_Bulb) != pr.values.end()) pick = SDK::CrShutterSpeed_Bulb;
            } else {
                pick = closest(pr.values, parseShutter(s), shutterSeconds);
            }
            bool ok = pick && pr.writable && cam.set(Code::CrDeviceProperty_ShutterSpeed, *pick, pr.type);
            results.push_back(result("shutter", ok, ok ? shutterLabel(*pick) : notSet(pr)));
        }

        // 5. ISO: bits 0-23 value (0xFFFFFF = AUTO), bits 24-27 mode (0 = normal).
        {
            Prop pr = cam.get(Code::CrDeviceProperty_IsoSensitivity);
            std::optional<std::uint64_t> pick;
            if (p.at("iso") == "auto") {
                for (auto v : pr.values) if ((v & 0xFFFFFF) == SDK::CrISO_AUTO && ((v >> 24) & 0xF) == SDK::CrISO_Normal) { pick = v; break; }
            } else {
                std::vector<std::uint64_t> normal;
                for (auto v : pr.values) if (((v >> 24) & 0xFF) == SDK::CrISO_Normal && (v & 0xFFFFFF) != SDK::CrISO_AUTO) normal.push_back(v);
                pick = closest(normal, p.at("iso").get<double>(), [](std::uint64_t v) { return std::optional<double>(double(v & 0xFFFFFF)); });
            }
            bool ok = pick && pr.writable && cam.set(Code::CrDeviceProperty_IsoSensitivity, *pick, pr.type);
            std::string label = pick ? (((*pick & 0xFFFFFF) == SDK::CrISO_AUTO) ? "ISO auto" : "ISO " + std::to_string(*pick & 0xFFFFFF)) : "";
            results.push_back(result("iso", ok, ok ? label : notSet(pr)));
        }

        // 6. White balance: preset, or Color Temp + Kelvin value.
        {
            const json& wb = p.at("wb");
            if (wb.is_number()) {
                bool ok = setFromList(Code::CrDeviceProperty_WhiteBalance, SDK::CrWhiteBalance_ColorTemp);
                std::string msg = "C.Temp not available";
                if (ok) {
                    std::this_thread::sleep_for(200ms);
                    Prop ct = cam.get(Code::CrDeviceProperty_Colortemp);
                    std::uint64_t k = wb.get<std::uint64_t>();
                    if (ct.values.size() >= 3 && ct.values[2] > 0) {  // range: min, max, step
                        k = std::clamp<std::uint64_t>(k, ct.values[0], ct.values[1]);
                        k = ct.values[0] + ((k - ct.values[0] + ct.values[2] / 2) / ct.values[2]) * ct.values[2];
                    }
                    ok = ct.writable && cam.set(Code::CrDeviceProperty_Colortemp, k, ct.type);
                    msg = ok ? std::to_string(k) + " K" : notSet(ct);
                }
                results.push_back(result("wb", ok, msg));
            } else {
                static const std::pair<const char*, std::uint64_t> names[] = {
                    {"auto", SDK::CrWhiteBalance_AWB}, {"daylight", SDK::CrWhiteBalance_Daylight},
                    {"shade", SDK::CrWhiteBalance_Shadow}, {"cloudy", SDK::CrWhiteBalance_Cloudy},
                    {"tungsten", SDK::CrWhiteBalance_Tungsten}, {"fluorescent", SDK::CrWhiteBalance_Fluorescent}};
                std::string n = wb.get<std::string>();
                std::uint64_t v = SDK::CrWhiteBalance_AWB;
                for (auto& [name, code] : names) if (n == name) v = code;
                bool ok = setFromList(Code::CrDeviceProperty_WhiteBalance, v);
                results.push_back(result("wb", ok, ok ? n : "not accepted"));
            }
        }

        // 7. Focus mode (the lens' own AF/MF switch, if it has one, still wins — reported by the bridge).
        {
            std::string f = p.at("focus");
            std::uint64_t v = f == "MF" ? SDK::CrFocus_MF : f == "AF-C" ? SDK::CrFocus_AF_C : SDK::CrFocus_AF_S;
            bool ok = setFromList(Code::CrDeviceProperty_FocusMode, v);
            results.push_back(result("focus", ok, ok ? f : "not accepted (check the lens AF/MF switch)"));
        }

        // 8. Drive mode.
        {
            std::string d = p.at("drive");
            std::uint64_t v = d == "cont_lo" ? SDK::CrDrive_Continuous_Lo
                            : d == "cont_hi_plus" ? SDK::CrDrive_Continuous_Hi_Plus
                            : d == "timer_2s" ? SDK::CrDrive_Timer_2s : SDK::CrDrive_Single;
            bool ok = setFromList(Code::CrDeviceProperty_DriveMode, v);
            results.push_back(result("drive", ok, ok ? d : "not accepted"));
        }

        return {{"results", results}, {"dial", currentDial()}};
    }

    std::string currentDial() {
        Prop pr = cam.get(Code::CrDeviceProperty_ExposureProgramMode);
        return pr.found ? dialName(pr.current) : "unknown";
    }

    bool releasePriority() {
        Prop pr = cam.get(Code::CrDeviceProperty_PriorityKeySettings);
        return pr.found && pr.writable &&
               cam.set(Code::CrDeviceProperty_PriorityKeySettings, SDK::CrPriorityKey_CameraPosition, pr.type);
    }

private:
    Camera& cam;

    void takePriority() {
        Prop pr = cam.get(Code::CrDeviceProperty_PriorityKeySettings);
        if (pr.found && pr.writable && pr.current != SDK::CrPriorityKey_PCRemote) {
            cam.set(Code::CrDeviceProperty_PriorityKeySettings, SDK::CrPriorityKey_PCRemote, pr.type);
            std::this_thread::sleep_for(500ms);
        }
    }

    // Sets an enum-like property, but only to a value the camera currently offers.
    bool setFromList(Code code, std::uint64_t value) {
        Prop pr = cam.get(code);
        if (!pr.found || !pr.writable) return false;
        if (!pr.values.empty() && std::find(pr.values.begin(), pr.values.end(), value) == pr.values.end()) return false;
        if (pr.current == value) return true;
        return cam.set(code, value, pr.type);  // same data type the camera reported, like Sony's RemoteCli
    }

    static std::string notSet(const Prop& pr) {
        if (!pr.found) return "not available";
        if (!pr.writable) return "locked by the camera right now";
        return "not accepted";
    }
};

// ---------------------------------------------------------------- main

static json handle(Camera& cam, const json& req) {
    std::string cmd = req.value("cmd", "");
    if (cmd == "status") {
        if (!cam.ensureOpen()) return {{"connected", false}, {"error", cam.lastError}};
        return {{"connected", true}, {"model", cam.model}, {"dial", Applier(cam).currentDial()}};
    }
    if (cmd == "apply") {
        if (!cam.ensureOpen()) return {{"error", cam.lastError}};
        try {
            return Applier(cam).apply(req.at("preset"));
        } catch (const std::exception& e) {
            return {{"error", std::string("invalid preset: ") + e.what()}};
        }
    }
    if (cmd == "release") {
        if (!cam.ensureOpen()) return {{"error", cam.lastError}};
        return {{"ok", Applier(cam).releasePriority()}};
    }
    return {{"error", "unknown command"}};
}

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "";
    if (mode != "serve" && mode != "status" && mode != "apply") {
        std::cerr << "usage: shotref-cam serve | status | apply < preset.json\n";
        return 2;
    }
    if (!SDK::Init()) { std::cerr << "Camera Remote SDK failed to initialise\n"; return 1; }
    int rc = 0;
    {
        Camera cam;
        if (mode == "serve") {
            std::string line;
            while (std::getline(std::cin, line)) {
                json answer;
                try { answer = handle(cam, json::parse(line)); }
                catch (const std::exception& e) { answer = {{"error", std::string("bad request: ") + e.what()}}; }
                std::cout << answer.dump() << std::endl;  // endl flushes: the bridge reads line by line
            }
        } else {
            json req = {{"cmd", mode}};
            if (mode == "apply") {
                try { std::cin >> req["preset"]; } catch (...) { std::cerr << "invalid preset JSON\n"; SDK::Release(); return 2; }
            }
            json answer = handle(cam, req);
            std::cout << answer.dump() << std::endl;
            if (mode == "apply" && answer.contains("error")) { std::cerr << answer["error"].get<std::string>() << "\n"; rc = 1; }
        }
    }
    SDK::Release();
    return rc;
}
