#pragma once
// VST3 implementation of the Plugin interface, built on the Steinberg VST3 SDK hosting
// helpers (module loading, PlugProvider, HostProcessData, EventList, ParameterChanges).
#include "bundle.hpp"
#include "plugin.hpp"

#include <memory>
#include <string>
#include <vector>

namespace wl {

// Plugin classes inside one .vst3 bundle. Loads the module, so run it in a child process
// when scanning unknown bundles (see catalog.cpp).
bool scanVst3Bundle(const std::string &path, std::vector<PluginInfo> &out, std::string &err);

class Vst3Plugin : public Plugin {
public:
    static std::unique_ptr<Plugin> create(const PluginInfo &info, std::string &err);
    ~Vst3Plugin() override;

    const std::string &id() const override { return id_; }
    const std::string &name() const override { return name_; }
    const char *format() const override { return "vst3"; }

    bool loadState(const StateFile &sf, std::string &err) override;
    bool loadPreset(const std::string &query, std::string &loadedName, std::string &err) override;
    std::vector<std::string> programs() override;
    bool saveStateFile(const std::string &path, size_t &bytes, std::string &err) override;
    std::vector<ParamInfo> params() const override;
    bool setParams(const std::vector<ParamValue> &values, std::string &err) override;
    bool commitParams(const std::vector<ParamValue> &values, double sampleRate, uint32_t block, std::string &err) override;
    bool render(const Job &job, const std::vector<TimedEvent> &events, const std::vector<ParamValue> &initial,
                const std::vector<AutoParam> &autos, const Audio *input, Audio &out, std::vector<std::string> &warnings,
                std::string &err) override;
    void pump(double ms) override;

    struct Impl;

private:
    Vst3Plugin();
    std::unique_ptr<Impl> impl_;
    std::string id_, name_;
};

} // namespace wl
