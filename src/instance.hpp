#pragma once
// One plugin instance plus the host side it talks to.
//
// Threading follows the CLAP rules: create/init/state/params-flush/activate happen on the
// main thread; start_processing/process/stop_processing happen on a dedicated audio thread.
// While audio renders, the main thread keeps pumping the run loop and on_main_thread()
// requests, because many plugins (JUCE ones especially) do real work there.
#include "bundle.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace wl {

struct ParamInfo {
    clap_id id;
    void *cookie;          // must be passed back in every event for this parameter
    std::string name, module;
    double min, max, def, value;
    bool stepped, readonly, hidden;
    std::string display;   // plugin's own text for the current value
};

struct ParamValue {
    clap_id id;
    void *cookie;
    double value;
};

class Instance {
public:
    static std::unique_ptr<Instance> create(std::shared_ptr<Bundle> bundle, const std::string &pluginId, std::string &err);
    ~Instance();

    const clap_plugin_t *plugin() const { return plugin_; }
    const std::string &id() const { return id_; }

    bool loadState(const std::vector<uint8_t> &data, std::string &err);
    bool saveState(std::vector<uint8_t> &out, std::string &err);

    std::vector<ParamInfo> params() const;
    // Resolve a parameter by id ("#123"), exact name, or "Module/Name" (case-insensitive).
    bool findParam(const std::string &key, ParamInfo &out) const;
    // Apply plain values on the main thread (plugin must be inactive).
    bool setParams(const std::vector<ParamValue> &values, std::string &err);

    bool activate(double sampleRate, uint32_t maxFrames, std::string &err);
    void deactivate();
    bool isActive() const { return active_; }

    // Output channel layout of the main output port; note dialect to use.
    uint32_t outputPortCount() const;
    uint32_t outputChannels(uint32_t port) const;
    uint32_t inputPortCount() const;
    uint32_t inputChannels(uint32_t port) const;
    bool usesMidiDialect() const;

    // Activate, process `blocks` silent blocks on an audio thread with these parameter
    // events in the first one, then deactivate. Some plugins (JUCE-based ones) only commit
    // parameter changes from process(), so do this before saving state.
    bool commitParams(const std::vector<ParamValue> &values, double sampleRate, uint32_t block,
                      int blocks, std::string &err);

    // Run the main-thread side for `ms` milliseconds (run loop + callback requests).
    void pump(double ms);
    void serviceCallbacks();

    // host-side state
    std::thread::id mainThread, audioThread;
    std::atomic<bool> callbackRequested{false}, restartRequested{false};
    std::vector<std::string> log;
    bool verbose = false;

private:
    Instance() = default;
    std::shared_ptr<Bundle> bundle_;
    std::string id_;
    clap_host_t host_{};
    const clap_plugin_t *plugin_ = nullptr;
    const clap_plugin_params_t *params_ = nullptr;
    const clap_plugin_state_t *state_ = nullptr;
    const clap_plugin_audio_ports_t *audioPorts_ = nullptr;
    const clap_plugin_note_ports_t *notePorts_ = nullptr;
    const clap_plugin_render_t *render_ = nullptr;
    bool active_ = false;

    void queryExtensions();
    static const void *hostGetExtension(const clap_host_t *host, const char *id);
    static void hostRequestRestart(const clap_host_t *host);
    static void hostRequestProcess(const clap_host_t *host);
    static void hostRequestCallback(const clap_host_t *host);
};

} // namespace wl
