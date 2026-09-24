#include "clap_plugin.hpp"

#include "presets.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <thread>

namespace wl {

namespace {
struct Events {
    std::vector<const clap_event_header_t *> ptrs;
    static uint32_t size(const clap_input_events_t *l) { return (uint32_t) static_cast<Events *>(l->ctx)->ptrs.size(); }
    static const clap_event_header_t *get(const clap_input_events_t *l, uint32_t i) { return static_cast<Events *>(l->ctx)->ptrs[i]; }
};
bool ignoreOut(const clap_output_events_t *, const clap_event_header_t *) { return true; }

clap_event_param_value_t paramEvent(ParamId id, void *cookie, double value, uint32_t at) {
    clap_event_param_value_t e{};
    e.header = {sizeof(e), at, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
    e.param_id = id; e.cookie = cookie;
    e.note_id = -1; e.port_index = -1; e.channel = -1; e.key = -1;
    e.value = value;
    return e;
}
} // namespace

std::unique_ptr<Plugin> ClapPlugin::create(const PluginInfo &info, std::string &err) {
    auto bundle = Bundle::open(info.bundlePath, err);
    if (!bundle) return nullptr;
    auto inst = Instance::create(bundle, info.id, err);
    if (!inst) return nullptr;
    std::unique_ptr<ClapPlugin> p(new ClapPlugin());
    p->inst_ = std::move(inst);
    p->id_ = info.id;
    p->name_ = info.name;
    p->bundle_ = info.bundlePath;
    return p;
}

bool ClapPlugin::loadState(const StateFile &sf, std::string &err) {
    if (sf.format == "vstpreset") { err = "a .vstpreset is VST3 state; " + name_ + " is a CLAP plugin"; return false; }
    if (!sf.pluginId.empty() && sf.format == "clap-preset" && sf.pluginId != id_) {
        err = "state file belongs to '" + sf.pluginId + "', not '" + id_ + "'";
        return false;
    }
    inst_->verbose = verbose;
    return inst_->loadState(sf.state, err);
}

bool ClapPlugin::loadPreset(const std::string &query, std::string &loadedName, std::string &err) {
    std::vector<PresetInfo> presets;
    if (!discoverPresets(bundle_, id_, presets, err)) return false;
    PresetInfo p;
    if (!findPreset(presets, query, p, err)) return false;
    inst_->verbose = verbose;
    if (!inst_->loadPreset(p.kind, p.location, p.loadKey, err)) { err = "preset '" + p.name + "': " + err; return false; }
    loadedName = p.category.empty() ? p.name : p.category + "/" + p.name;
    return true;
}

bool ClapPlugin::saveStateFile(const std::string &path, size_t &bytes, std::string &err) {
    std::vector<uint8_t> state;
    if (!inst_->saveState(state, err)) return false;
    bytes = state.size();
    return writeClapPreset(path, id_, state, err);
}

bool ClapPlugin::render(const Job &job, const std::vector<TimedEvent> &events, const std::vector<ParamValue> &initial,
                        const std::vector<AutoParam> &autos, const Audio *input, Audio &out,
                        std::vector<std::string> &warnings, std::string &err) {
    Instance &inst = *inst_;
    inst.verbose = verbose;
    const uint32_t block = (uint32_t)job.blockSize;
    const double sr = job.sampleRate;
    if (!inst.activate(sr, block, err)) return false;
    inst.pump((warmup >= 0 ? warmup : job.warmup) * 1000.0);
    const bool midi = inst.usesMidiDialect(), midiCtl = inst.acceptsMidi();
    bool warnedCc = false;

    const uint32_t outPorts = std::max<uint32_t>(1, inst.outputPortCount()), inPorts = inst.inputPortCount();
    std::vector<std::vector<std::vector<float>>> outStore(outPorts), inStore(inPorts);
    std::vector<std::vector<float *>> outPtrs(outPorts), inPtrs(inPorts);
    std::vector<clap_audio_buffer_t> outBufs(outPorts), inBufs(inPorts);
    for (uint32_t i = 0; i < outPorts; ++i) {
        const uint32_t ch = std::max<uint32_t>(1, inst.outputChannels(i));
        outStore[i].assign(ch, std::vector<float>(block, 0.f));
        for (auto &c : outStore[i]) outPtrs[i].push_back(c.data());
        outBufs[i] = {outPtrs[i].data(), nullptr, ch, 0, 0};
    }
    for (uint32_t i = 0; i < inPorts; ++i) {
        const uint32_t ch = std::max<uint32_t>(1, inst.inputChannels(i));
        inStore[i].assign(ch, std::vector<float>(block, 0.f));
        for (auto &c : inStore[i]) inPtrs[i].push_back(c.data());
        inBufs[i] = {inPtrs[i].data(), nullptr, ch, 0, 0};
    }
    if (input && inPorts == 0) { inst.deactivate(); err = name_ + " has no audio input, so it can't be used as an effect"; return false; }

    const int64_t total = (int64_t)out.frames();
    std::atomic<bool> done{false};
    std::string audioErr;
    std::thread worker([&] {
        inst.audioThread = std::this_thread::get_id();
        const clap_plugin_t *pl = inst.plugin();
        if (!pl->start_processing(pl)) { audioErr = "plugin refused to start processing"; done = true; return; }
        int64_t steady = 0;
        size_t next = 0;
        const int64_t warm = (int64_t)(0.1 * sr);
        std::vector<clap_event_note_t> notes;
        std::vector<clap_event_midi_t> midis;
        std::vector<clap_event_param_value_t> params;
        std::vector<clap_event_note_expression_t> exprs;
        bool resent = false;
        std::vector<double> lastAuto(autos.size(), NAN);
        clap_event_transport_t transport{};
        bool first = true;
        for (int64_t pos = -warm; pos < total && audioErr.empty();) {
            const uint32_t n = (uint32_t)std::min<int64_t>(block, pos < 0 ? -pos : total - pos);
            notes.clear(); midis.clear(); params.clear(); exprs.clear();
            params.reserve(initial.size() * 2 + autos.size());
            Events in;
            if (first) {   // parameter values again through process(): some plugins only commit them here
                for (const auto &v : initial) params.push_back(paramEvent(v.id, v.cookie, v.value, 0));
                first = false;
            }
            if (pos >= 0 && !resent) {   // again when audio starts: some plugins apply a loaded state late
                for (const auto &v : initial) params.push_back(paramEvent(v.id, v.cookie, v.value, 0));
                resent = true;
            }
            if (pos >= 0) {
                const double t = pos / sr;
                for (size_t a = 0; a < autos.size(); ++a) {
                    const double v = autos[a].env.at(t);
                    if (!(std::fabs(v - lastAuto[a]) < 1e-9)) { params.push_back(paramEvent(autos[a].id, autos[a].cookie, v, 0)); lastAuto[a] = v; }
                }
            }
            for (auto &e : params) in.ptrs.push_back(&e.header);
            if (pos >= 0) {
                const size_t firstEv = next;
                while (next < events.size() && events[next].frame < pos + n) ++next;
                notes.reserve(next - firstEv); midis.reserve(next - firstEv); exprs.reserve(next - firstEv);
                for (size_t i = firstEv; i < next; ++i) {
                    const auto &e = events[i];
                    const uint32_t at = (uint32_t)std::max<int64_t>(0, e.frame - pos);
                    if (e.kind != TimedEvent::Note) {
                        if (midiCtl) {
                            clap_event_midi_t m{};
                            m.header = {sizeof(m), at, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
                            if (e.kind == TimedEvent::CC) {
                                m.data[0] = (uint8_t)(0xB0 | (e.channel & 15)); m.data[1] = (uint8_t)e.number;
                                m.data[2] = (uint8_t)std::lround(e.value * 127);
                            } else if (e.kind == TimedEvent::Pressure) {
                                m.data[0] = (uint8_t)(0xD0 | (e.channel & 15)); m.data[1] = (uint8_t)std::lround(e.value * 127);
                            } else {
                                const int v = std::clamp((int)std::lround((e.value + 1) * 8192), 0, 16383);
                                m.data[0] = (uint8_t)(0xE0 | (e.channel & 15)); m.data[1] = (uint8_t)(v & 127); m.data[2] = (uint8_t)(v >> 7);
                            }
                            midis.push_back(m);
                            in.ptrs.push_back(&midis.back().header);
                        } else if (e.kind != TimedEvent::CC) {   // CLAP-only plugins: note expressions on every note
                            clap_event_note_expression_t x{};
                            x.header = {sizeof(x), at, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_EXPRESSION, 0};
                            x.expression_id = e.kind == TimedEvent::PitchBend ? CLAP_NOTE_EXPRESSION_TUNING : CLAP_NOTE_EXPRESSION_PRESSURE;
                            x.note_id = -1; x.port_index = 0; x.channel = -1; x.key = -1;
                            x.value = e.kind == TimedEvent::PitchBend ? e.value * e.range : e.value;
                            exprs.push_back(x);
                            in.ptrs.push_back(&exprs.back().header);
                        } else if (!warnedCc) {
                            warnedCc = true;
                            warnings.push_back(name_ + " takes no MIDI, so MIDI CC automation is ignored (automate its parameters instead)");
                        }
                        continue;
                    }
                    if (midi) {
                        clap_event_midi_t m{};
                        m.header = {sizeof(m), at, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
                        m.data[0] = (uint8_t)((e.on ? 0x90 : 0x80) | (e.channel & 15));
                        m.data[1] = (uint8_t)e.key;
                        m.data[2] = (uint8_t)std::clamp((int)std::lround(e.velocity * 127), 1, 127);
                        midis.push_back(m);
                        in.ptrs.push_back(&midis.back().header);
                    } else {
                        clap_event_note_t ne{};
                        ne.header = {sizeof(ne), at, CLAP_CORE_EVENT_SPACE_ID, uint16_t(e.on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0};
                        ne.note_id = -1; ne.port_index = 0; ne.channel = (int16_t)e.channel; ne.key = (int16_t)e.key;
                        ne.velocity = e.velocity;
                        notes.push_back(ne);
                        in.ptrs.push_back(&notes.back().header);
                    }
                }
            }
            // events must be time-ordered: parameter events sit at 0, notes follow
            std::stable_sort(in.ptrs.begin(), in.ptrs.end(), [](auto *a, auto *b) { return a->time < b->time; });

            const double sec = std::max<int64_t>(0, pos) / sr, beat = job.tempo.secToBeat(sec);
            const double barBeats = job.tsigNum * 4.0 / job.tsigDen;
            transport.header = {sizeof(transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
            transport.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE |
                              CLAP_TRANSPORT_HAS_TIME_SIGNATURE | (pos >= 0 ? CLAP_TRANSPORT_IS_PLAYING : 0);
            transport.song_pos_beats = (clap_beattime)std::llround(beat * CLAP_BEATTIME_FACTOR);
            transport.song_pos_seconds = (clap_sectime)std::llround(sec * CLAP_SECTIME_FACTOR);
            transport.tempo = job.tempo.bpmAtBeat(beat);
            transport.bar_number = (int32_t)std::floor(beat / barBeats);
            transport.bar_start = (clap_beattime)std::llround(transport.bar_number * barBeats * CLAP_BEATTIME_FACTOR);
            transport.tsig_num = (uint16_t)job.tsigNum;
            transport.tsig_denom = (uint16_t)job.tsigDen;

            for (auto &port : outStore) for (auto &c : port) std::fill(c.begin(), c.begin() + n, 0.f);
            for (auto &port : inStore) for (auto &c : port) std::fill(c.begin(), c.begin() + n, 0.f);
            if (input && pos >= 0 && !inStore.empty()) {
                auto &ip = inStore[0];
                for (uint32_t i = 0; i < n; ++i) {
                    ip[0][i] = input->left[pos + i];
                    if (ip.size() > 1) ip[1][i] = input->right[pos + i];
                }
            }
            clap_input_events_t inEvents{&in, &Events::size, &Events::get};
            clap_output_events_t outEvents{nullptr, &ignoreOut};
            clap_process_t proc{};
            proc.steady_time = steady;
            proc.frames_count = n;
            proc.transport = &transport;
            proc.audio_inputs = inBufs.empty() ? nullptr : inBufs.data();
            proc.audio_inputs_count = inPorts;
            proc.audio_outputs = outBufs.data();
            proc.audio_outputs_count = outPorts;
            proc.in_events = &inEvents;
            proc.out_events = &outEvents;
            if (pl->process(pl, &proc) == CLAP_PROCESS_ERROR) { audioErr = "plugin returned an error from process()"; break; }
            if (pos >= 0) {
                const auto &mainOut = outStore[0];
                for (uint32_t i = 0; i < n; ++i) {
                    out.left[pos + i] = mainOut[0][i];
                    out.right[pos + i] = mainOut.size() > 1 ? mainOut[1][i] : mainOut[0][i];
                }
            }
            steady += n;
            pos += n;
        }
        pl->stop_processing(pl);
        done = true;
    });
    while (!done) inst.pump(2);
    worker.join();
    inst.deactivate();
    for (auto &line : inst.log)
        if (line.rfind("error", 0) == 0 || line.rfind("fatal", 0) == 0 || line.rfind("warning", 0) == 0) warnings.push_back("plugin " + line);
    if (!audioErr.empty()) { err = audioErr; return false; }
    return true;
}

} // namespace wl
