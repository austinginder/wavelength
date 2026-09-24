#pragma once
// CLAP implementation of the Plugin interface (wraps Instance, the CLAP host side).
#include "bundle.hpp"
#include "instance.hpp"
#include "plugin.hpp"

#include <memory>

namespace wl {

class ClapPlugin : public Plugin {
public:
    static std::unique_ptr<Plugin> create(const PluginInfo &info, std::string &err);

    const std::string &id() const override { return id_; }
    const std::string &name() const override { return name_; }
    const char *format() const override { return "clap"; }

    bool loadState(const StateFile &sf, std::string &err) override;
    bool saveStateFile(const std::string &path, size_t &bytes, std::string &err) override;
    std::vector<ParamInfo> params() const override { return inst_->params(); }
    bool setParams(const std::vector<ParamValue> &values, std::string &err) override { return inst_->setParams(values, err); }
    bool commitParams(const std::vector<ParamValue> &values, double sampleRate, uint32_t block, std::string &err) override {
        return inst_->commitParams(values, sampleRate, block, 8, err);
    }
    bool render(const Job &job, const std::vector<TimedEvent> &events, const std::vector<ParamValue> &initial,
                const std::vector<AutoParam> &autos, const Audio *input, Audio &out, std::vector<std::string> &warnings,
                std::string &err) override;
    void pump(double ms) override { inst_->verbose = verbose; inst_->pump(ms); }

private:
    std::unique_ptr<Instance> inst_;
    std::string id_, name_;
};

} // namespace wl
