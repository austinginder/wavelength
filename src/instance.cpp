#include "instance.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "platform.hpp"

namespace wl {

namespace {

Instance *self(const clap_host_t *h) { return static_cast<Instance *>(h->host_data); }

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// ---- host extensions -------------------------------------------------------------------
const clap_host_thread_check_t kThreadCheck = {
    [](const clap_host_t *h) { return std::this_thread::get_id() == self(h)->mainThread; },
    [](const clap_host_t *h) { return std::this_thread::get_id() == self(h)->audioThread; },
};

const clap_host_log_t kLog = {
    [](const clap_host_t *h, clap_log_severity severity, const char *msg) {
        static const char *names[] = {"debug", "info", "warning", "error", "fatal", "host-misbehaving", "plugin-misbehaving"};
        std::string line = std::string(severity >= 0 && severity <= 6 ? names[severity] : "log") + ": " + (msg ? msg : "");
        self(h)->log.push_back(line);
        if (self(h)->verbose) std::fprintf(stderr, "[plugin] %s\n", line.c_str());
    },
};

const clap_host_params_t kParams = {
    [](const clap_host_t *, clap_param_rescan_flags) {},
    [](const clap_host_t *, clap_id, clap_param_clear_flags) {},
    [](const clap_host_t *) {},
};

const clap_host_state_t kState = {[](const clap_host_t *) {}};
const clap_host_latency_t kLatency = {[](const clap_host_t *) {}};
const clap_host_tail_t kTail = {[](const clap_host_t *) {}};

const clap_host_note_ports_t kNotePorts = {
    [](const clap_host_t *) -> uint32_t { return CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI; },
    [](const clap_host_t *, uint32_t) {},
};

const clap_host_preset_load_t kPresetLoad = {
    [](const clap_host_t *h, uint32_t, const char *, const char *, int32_t, const char *msg) { self(h)->presetError = msg ? msg : "preset load failed"; },
    [](const clap_host_t *, uint32_t, const char *, const char *) {},
};

const clap_host_audio_ports_t kAudioPorts = {
    [](const clap_host_t *, uint32_t) { return false; },
    [](const clap_host_t *, uint32_t) {},
};

// ---- streams ---------------------------------------------------------------------------
struct ReadCtx { const std::vector<uint8_t> *data; size_t pos; };
int64_t streamRead(const clap_istream_t *s, void *buf, uint64_t size) {
    auto *c = static_cast<ReadCtx *>(s->ctx);
    uint64_t n = std::min<uint64_t>(size, c->data->size() - c->pos);
    std::memcpy(buf, c->data->data() + c->pos, n);
    c->pos += n;
    return (int64_t)n;
}
int64_t streamWrite(const clap_ostream_t *s, const void *buf, uint64_t size) {
    auto *v = static_cast<std::vector<uint8_t> *>(s->ctx);
    auto *p = static_cast<const uint8_t *>(buf);
    v->insert(v->end(), p, p + size);
    return (int64_t)size;
}

// ---- event lists -----------------------------------------------------------------------
struct EventList {
    std::vector<const clap_event_header_t *> events;
    static uint32_t size(const clap_input_events_t *l) { return (uint32_t)static_cast<EventList *>(l->ctx)->events.size(); }
    static const clap_event_header_t *get(const clap_input_events_t *l, uint32_t i) { return static_cast<EventList *>(l->ctx)->events[i]; }
};
bool dropEvent(const clap_output_events_t *, const clap_event_header_t *) { return true; }

} // namespace

// ---- lifecycle ---------------------------------------------------------------------------
std::unique_ptr<Instance> Instance::create(std::shared_ptr<Bundle> bundle, const std::string &pluginId, std::string &err) {
    std::unique_ptr<Instance> inst(new Instance());
    inst->bundle_ = bundle;
    inst->id_ = pluginId;
    inst->mainThread = std::this_thread::get_id();
    inst->host_ = clap_host_t{CLAP_VERSION, inst.get(), "Wavelength", "wavelength.run", "https://wavelength.run", WAVELENGTH_VERSION,
                              &Instance::hostGetExtension, &Instance::hostRequestRestart,
                              &Instance::hostRequestProcess, &Instance::hostRequestCallback};
    inst->plugin_ = bundle->factory()->create_plugin(bundle->factory(), &inst->host_, pluginId.c_str());
    if (!inst->plugin_) {
        err = "factory could not create plugin '" + pluginId + "'";
        return nullptr;
    }
    if (!inst->plugin_->init(inst->plugin_)) {
        err = "plugin '" + pluginId + "' failed to initialise";
        inst->plugin_->destroy(inst->plugin_);
        inst->plugin_ = nullptr;
        return nullptr;
    }
    inst->queryExtensions();
    // Offline rendering: let plugins pick their best-quality, non-realtime path.
    if (inst->render_) inst->render_->set(inst->plugin_, CLAP_RENDER_OFFLINE);
    return inst;
}

Instance::~Instance() {
    if (!plugin_) return;
    if (active_) deactivate();
    plugin_->destroy(plugin_);
}

void Instance::queryExtensions() {
    auto ext = [&](const char *id) { return plugin_->get_extension(plugin_, id); };
    params_ = static_cast<const clap_plugin_params_t *>(ext(CLAP_EXT_PARAMS));
    state_ = static_cast<const clap_plugin_state_t *>(ext(CLAP_EXT_STATE));
    audioPorts_ = static_cast<const clap_plugin_audio_ports_t *>(ext(CLAP_EXT_AUDIO_PORTS));
    latency_ = static_cast<const clap_plugin_latency_t *>(ext(CLAP_EXT_LATENCY));
    notePorts_ = static_cast<const clap_plugin_note_ports_t *>(ext(CLAP_EXT_NOTE_PORTS));
    render_ = static_cast<const clap_plugin_render_t *>(ext(CLAP_EXT_RENDER));
    presetLoad_ = static_cast<const clap_plugin_preset_load_t *>(ext(CLAP_EXT_PRESET_LOAD));
    if (!presetLoad_) presetLoad_ = static_cast<const clap_plugin_preset_load_t *>(ext(CLAP_EXT_PRESET_LOAD_COMPAT));
}

const void *Instance::hostGetExtension(const clap_host_t *, const char *id) {
    if (!std::strcmp(id, CLAP_EXT_THREAD_CHECK)) return &kThreadCheck;
    if (!std::strcmp(id, CLAP_EXT_LOG)) return &kLog;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kLatency;
    if (!std::strcmp(id, CLAP_EXT_TAIL)) return &kTail;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
    if (!std::strcmp(id, CLAP_EXT_PRESET_LOAD) || !std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT)) return &kPresetLoad;
    return nullptr;
}
void Instance::hostRequestRestart(const clap_host_t *h) { self(h)->restartRequested = true; }
void Instance::hostRequestProcess(const clap_host_t *) {}
void Instance::hostRequestCallback(const clap_host_t *h) { self(h)->callbackRequested = true; }

void Instance::serviceCallbacks() {
    if (callbackRequested.exchange(false)) plugin_->on_main_thread(plugin_);
}

void Instance::pump(double ms) {
    auto until = std::chrono::steady_clock::now() + std::chrono::microseconds((int64_t)(ms * 1000));
    do {
        serviceCallbacks();
        platform::pumpEvents(2);
    } while (std::chrono::steady_clock::now() < until);
    serviceCallbacks();
}

// ---- state -------------------------------------------------------------------------------
bool Instance::loadState(const std::vector<uint8_t> &data, std::string &err) {
    if (!state_) { err = "plugin has no state extension"; return false; }
    ReadCtx ctx{&data, 0};
    clap_istream_t s{&ctx, &streamRead};
    if (!state_->load(plugin_, &s)) { err = "plugin rejected the state (" + std::to_string(data.size()) + " bytes)"; return false; }
    return true;
}

bool Instance::saveState(std::vector<uint8_t> &out, std::string &err) {
    if (!state_) { err = "plugin has no state extension"; return false; }
    out.clear();
    clap_ostream_t s{&out, &streamWrite};
    if (!state_->save(plugin_, &s)) { err = "plugin failed to save its state"; return false; }
    return true;
}

bool Instance::loadPreset(uint32_t kind, const std::string &location, const std::string &loadKey, std::string &err) {
    if (!presetLoad_) { err = "plugin has no preset-load extension"; return false; }
    presetError.clear();
    const bool ok = presetLoad_->from_location(plugin_, kind, location.empty() ? nullptr : location.c_str(),
                                               loadKey.empty() ? nullptr : loadKey.c_str());
    pump(100);   // plugins may finish loading on the main thread
    if (!ok || !presetError.empty()) { err = presetError.empty() ? "the plugin could not load the preset" : presetError; return false; }
    return true;
}

// ---- parameters --------------------------------------------------------------------------
std::vector<ParamInfo> Instance::params() const {
    std::vector<ParamInfo> out;
    if (!params_) return out;
    uint32_t n = params_->count(plugin_);
    for (uint32_t i = 0; i < n; ++i) {
        clap_param_info_t info{};
        if (!params_->get_info(plugin_, i, &info)) continue;
        ParamInfo p;
        p.id = info.id;
        p.cookie = info.cookie;
        p.name = info.name;
        p.module = info.module;
        p.min = info.min_value;
        p.max = info.max_value;
        p.def = info.default_value;
        p.value = info.default_value;
        params_->get_value(plugin_, info.id, &p.value);
        p.stepped = info.flags & CLAP_PARAM_IS_STEPPED;
        p.readonly = info.flags & CLAP_PARAM_IS_READONLY;
        p.hidden = info.flags & CLAP_PARAM_IS_HIDDEN;
        char text[256] = {0};
        if (params_->value_to_text && params_->value_to_text(plugin_, info.id, p.value, text, sizeof text)) p.display = text;
        out.push_back(std::move(p));
    }
    return out;
}

bool Instance::findParam(const std::string &key, ParamInfo &out) const {
    auto all = params();
    if (!key.empty() && key[0] == '#') {
        clap_id want = (clap_id)std::stoul(key.substr(1));
        for (auto &p : all) if (p.id == want) { out = p; return true; }
        return false;
    }
    std::string k = lower(key);
    for (auto &p : all) if (lower(p.name) == k) { out = p; return true; }
    for (auto &p : all) if (lower(p.module + "/" + p.name) == k) { out = p; return true; }
    return false;
}

bool Instance::setParams(const std::vector<ParamValue> &values, std::string &err) {
    if (values.empty()) return true;
    if (!params_) { err = "plugin has no parameters extension"; return false; }
    std::vector<clap_event_param_value_t> evs(values.size());
    EventList list;
    for (size_t i = 0; i < values.size(); ++i) {
        auto &e = evs[i];
        e.header = {sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        e.param_id = values[i].id;
        e.cookie = values[i].cookie;
        e.note_id = -1; e.port_index = -1; e.channel = -1; e.key = -1;
        e.value = values[i].value;
        list.events.push_back(&e.header);
    }
    clap_input_events_t in{&list, &EventList::size, &EventList::get};
    clap_output_events_t out{nullptr, &dropEvent};
    params_->flush(plugin_, &in, &out);
    return true;
}

bool Instance::commitParams(const std::vector<ParamValue> &values, double sampleRate, uint32_t block,
                            int blocks, std::string &err) {
    if (!activate(sampleRate, block, err)) return false;
    pump(50);
    const uint32_t outPorts = std::max<uint32_t>(1, outputPortCount()), inPorts = inputPortCount();
    std::vector<std::vector<std::vector<float>>> store(outPorts + inPorts);
    std::vector<std::vector<float *>> ptrs(outPorts + inPorts);
    std::vector<clap_audio_buffer_t> bufs(outPorts + inPorts);
    for (uint32_t p = 0; p < outPorts + inPorts; ++p) {
        uint32_t ch = std::max<uint32_t>(1, p < outPorts ? outputChannels(p) : inputChannels(p - outPorts));
        store[p].assign(ch, std::vector<float>(block, 0.f));
        for (auto &c : store[p]) ptrs[p].push_back(c.data());
        bufs[p] = {ptrs[p].data(), nullptr, ch, 0, 0};
    }
    std::vector<clap_event_param_value_t> evs(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        auto &e = evs[i];
        e.header = {sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        e.param_id = values[i].id; e.cookie = values[i].cookie;
        e.note_id = -1; e.port_index = -1; e.channel = -1; e.key = -1;
        e.value = values[i].value;
    }
    std::atomic<bool> done{false};
    bool ok = true;
    std::thread worker([&] {
        audioThread = std::this_thread::get_id();
        if (!plugin_->start_processing(plugin_)) { ok = false; done = true; return; }
        for (int b = 0; b < blocks; ++b) {
            EventList list;
            if (b == 0) for (auto &e : evs) list.events.push_back(&e.header);
            clap_input_events_t in{&list, &EventList::size, &EventList::get};
            clap_output_events_t out{nullptr, &dropEvent};
            // a stopped transport: some plugins read it without checking for null
            clap_event_transport_t transport{};
            transport.header = {sizeof(transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
            transport.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
            transport.tempo = 120;
            transport.tsig_num = 4;
            transport.tsig_denom = 4;
            clap_process_t proc{};
            proc.transport = &transport;
            proc.steady_time = (int64_t)b * block;
            proc.frames_count = block;
            proc.audio_outputs = bufs.data();
            proc.audio_outputs_count = outPorts;
            proc.audio_inputs = inPorts ? bufs.data() + outPorts : nullptr;
            proc.audio_inputs_count = inPorts;
            proc.in_events = &in;
            proc.out_events = &out;
            plugin_->process(plugin_, &proc);
        }
        plugin_->stop_processing(plugin_);
        done = true;
    });
    while (!done) pump(2);
    worker.join();
    pump(50);
    deactivate();
    if (!ok) err = "plugin refused to start processing";
    return ok;
}

// ---- activation & layout -----------------------------------------------------------------
bool Instance::activate(double sampleRate, uint32_t maxFrames, std::string &err) {
    if (!plugin_->activate(plugin_, sampleRate, 1, maxFrames)) { err = "plugin refused to activate"; return false; }
    active_ = true;
    return true;
}

void Instance::deactivate() {
    if (!active_) return;
    plugin_->deactivate(plugin_);
    active_ = false;
}

uint32_t Instance::outputPortCount() const { return audioPorts_ ? audioPorts_->count(plugin_, false) : 1; }
uint32_t Instance::inputPortCount() const { return audioPorts_ ? audioPorts_->count(plugin_, true) : 0; }
uint32_t Instance::outputChannels(uint32_t port) const {
    if (!audioPorts_) return 2;
    clap_audio_port_info_t info{};
    return audioPorts_->get(plugin_, port, false, &info) ? info.channel_count : 2;
}
uint32_t Instance::inputChannels(uint32_t port) const {
    if (!audioPorts_) return 0;
    clap_audio_port_info_t info{};
    return audioPorts_->get(plugin_, port, true, &info) ? info.channel_count : 0;
}
bool Instance::textToValue(uint32_t id, const std::string &text, double &value) const {
    return params_ && params_->text_to_value && params_->text_to_value(plugin_, id, text.c_str(), &value);
}
bool Instance::valueToText(uint32_t id, double value, std::string &text) const {
    char buf[256] = {};
    if (!params_ || !params_->value_to_text || !params_->value_to_text(plugin_, id, value, buf, sizeof buf)) return false;
    buf[sizeof buf - 1] = 0;
    text = buf;
    return true;
}
uint32_t Instance::latency() const { return latency_ ? latency_->get(plugin_) : 0; }
bool Instance::acceptsMidi() const {
    if (!notePorts_ || notePorts_->count(plugin_, true) == 0) return false;
    clap_note_port_info_t info{};
    return notePorts_->get(plugin_, 0, true, &info) && (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI);
}
bool Instance::usesMidiDialect() const {
    if (!notePorts_ || notePorts_->count(plugin_, true) == 0) return false;
    clap_note_port_info_t info{};
    if (!notePorts_->get(plugin_, 0, true, &info)) return false;
    if (info.supported_dialects & CLAP_NOTE_DIALECT_CLAP) return false;
    return info.supported_dialects & CLAP_NOTE_DIALECT_MIDI;
}

} // namespace wl
