#pragma once
// Audio Unit (AUv2, macOS) implementation of the Plugin interface, on AudioToolbox's C API.
// Instruments ('aumu'), effects ('aufx') and MIDI-controlled effects ('aumf'). A component that
// won't open in this process (an Intel-only plugin on Apple silicon, an app-extension AUv3) is
// opened out of process by the system. State is the unit's ClassInfo property list, which is the
// .aupreset format; factory presets are its programs. Elsewhere this compiles to stubs.
#include "bundle.hpp"
#include "plugin.hpp"

#include <memory>
#include <string>
#include <vector>

namespace wl {

// Every instrument and effect Audio Unit in the system's component registry (no plugin code runs).
std::vector<PluginInfo> auPlugins();

class AuPlugin : public Plugin {
public:
    static std::unique_ptr<Plugin> create(const PluginInfo &info, std::string &err);
    ~AuPlugin() override;

    const std::string &id() const override { return id_; }
    const std::string &name() const override { return name_; }
    const char *format() const override { return "au"; }

    bool loadState(const StateFile &sf, std::string &err) override;
    bool loadPreset(const std::string &query, std::string &loadedName, std::string &err) override;
    std::vector<std::string> programs() override;
    bool saveStateFile(const std::string &path, size_t &bytes, std::string &err) override;
    bool getState(std::vector<uint8_t> &out, std::string &err) override;
    bool valueFromText(ParamId id, const std::string &text, double &plain) override;
    bool textForValue(ParamId id, double plain, std::string &text) override;
    std::vector<ParamInfo> params() const override;
    bool setParams(const std::vector<ParamValue> &values, std::string &err) override;
    bool commitParams(const std::vector<ParamValue> &values, double sampleRate, uint32_t block, std::string &err) override;
    bool render(const Job &job, const std::vector<TimedEvent> &events, const std::vector<ParamValue> &initial,
                const std::vector<AutoParam> &autos, const Audio *input, Audio &out, std::vector<std::string> &warnings,
                std::string &err) override;
    void pump(double ms) override;

    struct Impl;

private:
    AuPlugin();
    std::unique_ptr<Impl> impl_;
    std::string id_, name_;
};

} // namespace wl
