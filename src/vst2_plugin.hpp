#pragma once
// VST 2 implementation of the Plugin interface: loads a .vst bundle (macOS), .dll (Windows) or
// .so (Linux) through its VSTPluginMain entry, on Wavelength's own declaration of the VST 2 ABI
// (vst2_abi.hpp). State comes from .fxp/.fxb files (opaque chunks or parameter lists).
#include "bundle.hpp"
#include "plugin.hpp"

#include <memory>
#include <string>
#include <vector>

namespace wl {

// Plugins inside one VST 2 library. Runs plugin code, so scan unknown libraries in a child
// process (see catalog.cpp). Shell plugins (several plugins behind one library) are reported
// as unsupported.
bool scanVst2Bundle(const std::string &path, std::vector<PluginInfo> &out, std::string &err);

class Vst2Plugin : public Plugin {
public:
    static std::unique_ptr<Plugin> create(const PluginInfo &info, std::string &err);
    ~Vst2Plugin() override;

    const std::string &id() const override { return id_; }
    const std::string &name() const override { return name_; }
    const char *format() const override { return "vst2"; }

    bool loadState(const StateFile &sf, std::string &err) override;
    bool loadPreset(const std::string &query, std::string &loadedName, std::string &err) override;
    std::vector<std::string> programs() override;
    bool saveStateFile(const std::string &path, size_t &bytes, std::string &err) override;
    bool getState(std::vector<uint8_t> &out, std::string &err) override;
    bool valueFromText(ParamId id, const std::string &text, double &plain) override;
    std::vector<ParamInfo> params() const override;
    bool setParams(const std::vector<ParamValue> &values, std::string &err) override;
    bool commitParams(const std::vector<ParamValue> &values, double sampleRate, uint32_t block, std::string &err) override;
    bool render(const Job &job, const std::vector<TimedEvent> &events, const std::vector<ParamValue> &initial,
                const std::vector<AutoParam> &autos, const Audio *input, Audio &out, std::vector<std::string> &warnings,
                std::string &err) override;
    void pump(double ms) override;

    struct Impl;

private:
    Vst2Plugin();
    std::unique_ptr<Impl> impl_;
    std::string id_, name_;
};

} // namespace wl
