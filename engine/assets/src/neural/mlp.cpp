#include <oxwald/assets/neural/mlp.hpp>
#include <oxwald/core/assert.hpp>
#include <oxwald/core/time.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <numbers>
#include <thread>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#define OX_NN_HAS_MXCSR 1
#endif

namespace ox::nn {

namespace {
// Denormals make training ~10x slower (Adam moments of untouched latents decay into the denormal range): workers run
// with flush-to-zero / denormals-are-zero; the caller's mode is restored after each run().
struct ScopedFlushDenormals {
#if OX_NN_HAS_MXCSR
    unsigned int saved = _mm_getcsr();
    ScopedFlushDenormals() { _mm_setcsr(saved | 0x8040u); }
    ~ScopedFlushDenormals() { _mm_setcsr(saved); }
#endif
};
} // namespace

// ---- half ----------------------------------------------------------------------------------------------------

u16 floatToHalf(f32 value) {
    const u32 x = std::bit_cast<u32>(value);
    const u32 sign = (x >> 16) & 0x8000u;
    const u32 absx = x & 0x7FFFFFFFu;
    if (absx >= 0x7F800000u) return u16(sign | (absx > 0x7F800000u ? 0x7E00u : 0x7C00u)); // NaN / Inf
    if (absx >= 0x477FF000u) return u16(sign | 0x7BFFu);                                 // saturate to 65504
    if (absx < 0x38800000u) {                                                             // subnormal half
        // value * 2^24 rounded to nearest even gives the subnormal mantissa.
        const f32 f = std::bit_cast<f32>(absx) * 16777216.0f;
        const f32 r = std::nearbyint(f);
        return u16(sign | u32(r));
    }
    const u32 mant = absx & 0x007FFFFFu;
    u32 h = ((absx >> 23) - 112u) << 10 | (mant >> 13);
    const u32 rest = mant & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (h & 1u))) ++h;
    return u16(sign | h);
}

f32 halfToFloat(u16 h) {
    const u32 sign = u32(h & 0x8000u) << 16;
    const u32 exp = (h >> 10) & 0x1Fu;
    const u32 mant = h & 0x3FFu;
    if (exp == 0) {
        const f32 f = f32(mant) * (1.0f / 16777216.0f);
        return sign ? -f : f;
    }
    if (exp == 31) return std::bit_cast<f32>(sign | 0x7F800000u | (mant << 13));
    return std::bit_cast<f32>(sign | ((exp + 112u) << 23) | (mant << 13));
}

// ---- rng -----------------------------------------------------------------------------------------------------

Pcg32::Pcg32(u64 seed, u64 stream) {
    m_inc = (stream << 1u) | 1u;
    m_state = 0;
    next();
    m_state += seed;
    next();
}

u32 Pcg32::next() {
    const u64 old = m_state;
    m_state = old * 6364136223846793005ull + m_inc;
    const u32 xorshifted = u32(((old >> 18u) ^ old) >> 27u);
    const u32 rot = u32(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
}

u32 Pcg32::below(u32 bound) {
    if (bound <= 1) return 0;
    const u32 threshold = (0u - bound) % bound;
    for (;;) {
        const u32 r = next();
        if (r >= threshold) return r % bound;
    }
}

// ---- mlp -----------------------------------------------------------------------------------------------------

namespace {

inline void axpy(f32* __restrict y, f32 a, const f32* __restrict x, u32 n) {
    for (u32 i = 0; i < n; ++i) y[i] += a * x[i];
}

inline void activate(f32* v, u32 n, Activation act) {
    if (act == Activation::ReLU) {
        for (u32 i = 0; i < n; ++i) v[i] = v[i] > 0.0f ? v[i] : 0.0f;
    }
}

template <class T>
void put(std::vector<std::byte>& out, const T& v) {
    const auto* p = reinterpret_cast<const std::byte*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}
template <class T>
bool get(std::span<const std::byte>& in, T& v) {
    if (in.size() < sizeof(T)) return false;
    std::memcpy(&v, in.data(), sizeof(T));
    in = in.subspan(sizeof(T));
    return true;
}

} // namespace

Mlp::Mlp(std::vector<LayerDesc> layers) : m_layers(std::move(layers)) { layout(); }

Mlp Mlp::make(u32 inputs, std::initializer_list<u32> hidden, u32 outputs, Activation hiddenActivation,
              Activation outputActivation) {
    return make(inputs, std::span<const u32>(hidden.begin(), hidden.size()), outputs, hiddenActivation,
                outputActivation);
}

Mlp Mlp::make(u32 inputs, std::span<const u32> hidden, u32 outputs, Activation hiddenActivation,
              Activation outputActivation) {
    std::vector<LayerDesc> layers;
    u32 prev = inputs;
    for (u32 h : hidden) {
        layers.push_back({prev, h, hiddenActivation});
        prev = h;
    }
    layers.push_back({prev, outputs, outputActivation});
    return Mlp(std::move(layers));
}

void Mlp::layout() {
    m_offsets.clear();
    usize total = 0;
    m_maxWidth = 0;
    for (const LayerDesc& l : m_layers) {
        m_offsets.push_back(total);
        total += usize(l.inputs) * l.outputs + l.outputs;
        m_maxWidth = std::max({m_maxWidth, l.inputs, l.outputs});
    }
    m_params.assign(total, 0.0f);
    m_transposed.assign(total, 0.0f);
}

u64 Mlp::macsPerSample() const {
    u64 n = 0;
    for (const LayerDesc& l : m_layers) n += u64(l.inputs) * l.outputs;
    return n;
}

void Mlp::initialize(u64 seed) {
    Pcg32 rng(seed, 0x9e3779b97f4a7c15ull);
    for (u32 li = 0; li < m_layers.size(); ++li) {
        const LayerDesc& l = m_layers[li];
        const f32 bound = std::sqrt(6.0f / f32(std::max(l.inputs, 1u))) *
                          (l.activation == Activation::ReLU ? 1.0f : 0.5f);
        f32* w = &m_params[m_offsets[li]];
        for (usize i = 0; i < usize(l.inputs) * l.outputs; ++i) w[i] = rng.uniform(-bound, bound);
        std::fill_n(w + usize(l.inputs) * l.outputs, l.outputs, 0.0f);
    }
    syncTransposed();
}

void Mlp::roundToHalf() {
    for (f32& p : m_params) p = nn::roundToHalf(p);
    syncTransposed();
}

void Mlp::syncTransposed() {
    for (u32 li = 0; li < m_layers.size(); ++li) {
        const LayerDesc& l = m_layers[li];
        const f32* w = &m_params[m_offsets[li]];
        f32* t = &m_transposed[m_offsets[li]];
        for (u32 o = 0; o < l.outputs; ++o)
            for (u32 i = 0; i < l.inputs; ++i) t[usize(i) * l.outputs + o] = w[usize(o) * l.inputs + i];
    }
}

void Mlp::forward(const f32* in, f32* out, f32* scratch) const {
    const f32* x = in;
    f32* bufs[2] = {scratch, scratch + m_maxWidth};
    for (u32 li = 0; li < m_layers.size(); ++li) {
        const LayerDesc& l = m_layers[li];
        f32* y = li + 1 == m_layers.size() ? out : bufs[li & 1u];
        const f32* t = &m_transposed[m_offsets[li]];
        std::memcpy(y, &m_params[biasOffset(li)], sizeof(f32) * l.outputs);
        for (u32 i = 0; i < l.inputs; ++i) {
            const f32 a = x[i];
            if (a != 0.0f) axpy(y, a, t + usize(i) * l.outputs, l.outputs);
        }
        activate(y, l.outputs, l.activation);
        x = y;
    }
}

void Mlp::forwardBatch(const f32* in, u32 count, f32* out) const {
    std::vector<f32> scratch(scratchSize());
    for (u32 s = 0; s < count; ++s) forward(in + usize(s) * inputs(), out + usize(s) * outputs(), scratch.data());
}

void Mlp::forwardTrain(const f32* in, u32 count, Tape& tape, f32* out) const {
    usize total = 0;
    for (const LayerDesc& l : m_layers) total += usize(l.inputs) * count;
    total += usize(outputs()) * count;
    tape.count = count;
    if (tape.values.size() < total) tape.values.resize(total);
    std::memcpy(tape.values.data(), in, sizeof(f32) * usize(inputs()) * count);
    usize offset = 0;
    for (u32 li = 0; li < m_layers.size(); ++li) {
        const LayerDesc& l = m_layers[li];
        const f32* x = tape.values.data() + offset;
        f32* y = tape.values.data() + offset + usize(l.inputs) * count;
        const f32* t = &m_transposed[m_offsets[li]];
        const f32* b = &m_params[biasOffset(li)];
        for (u32 s = 0; s < count; ++s) {
            f32* ys = y + usize(s) * l.outputs;
            const f32* xs = x + usize(s) * l.inputs;
            std::memcpy(ys, b, sizeof(f32) * l.outputs);
            for (u32 i = 0; i < l.inputs; ++i) {
                const f32 a = xs[i];
                if (a != 0.0f) axpy(ys, a, t + usize(i) * l.outputs, l.outputs);
            }
            activate(ys, l.outputs, l.activation);
        }
        offset += usize(l.inputs) * count;
    }
    if (out) std::memcpy(out, tape.values.data() + offset, sizeof(f32) * usize(outputs()) * count);
}

const f32* Mlp::tapeOutputs(const Tape& tape) const {
    usize offset = 0;
    for (const LayerDesc& l : m_layers) offset += usize(l.inputs) * tape.count;
    return tape.values.data() + offset;
}

void Mlp::backward(Tape& tape, const f32* dOut, f32* grad, f32* dIn) const {
    const u32 count = tape.count;
    std::vector<usize> offsets(m_layers.size() + 1);
    usize offset = 0;
    for (u32 li = 0; li < m_layers.size(); ++li) {
        offsets[li] = offset;
        offset += usize(m_layers[li].inputs) * count;
    }
    offsets[m_layers.size()] = offset;
    tape.delta.assign(dOut, dOut + usize(outputs()) * count);
    for (u32 li = u32(m_layers.size()); li-- > 0;) {
        const LayerDesc& l = m_layers[li];
        const f32* aIn = tape.values.data() + offsets[li];
        const f32* aOut = tape.values.data() + offsets[li + 1];
        f32* delta = tape.delta.data();
        if (l.activation == Activation::ReLU) {
            for (usize i = 0; i < usize(count) * l.outputs; ++i)
                if (aOut[i] <= 0.0f) delta[i] = 0.0f;
        }
        f32* gw = grad + m_offsets[li];
        f32* gb = gw + usize(l.inputs) * l.outputs;
        const f32* w = &m_params[m_offsets[li]];
        const bool needInput = li > 0 || dIn;
        f32* next = nullptr;
        if (needInput) {
            f32* target = li > 0 ? nullptr : dIn;
            if (!target) {
                tape.delta2.assign(usize(count) * l.inputs, 0.0f);
                target = tape.delta2.data();
            } else {
                std::fill_n(target, usize(count) * l.inputs, 0.0f);
            }
            next = target;
        }
        for (u32 s = 0; s < count; ++s) {
            const f32* ds = delta + usize(s) * l.outputs;
            const f32* xs = aIn + usize(s) * l.inputs;
            f32* ns = next ? next + usize(s) * l.inputs : nullptr;
            for (u32 o = 0; o < l.outputs; ++o) {
                const f32 d = ds[o];
                if (d == 0.0f) continue;
                gb[o] += d;
                axpy(gw + usize(o) * l.inputs, d, xs, l.inputs);
                if (ns) axpy(ns, d, w + usize(o) * l.inputs, l.inputs);
            }
        }
        if (li > 0) tape.delta.swap(tape.delta2);
    }
}

std::vector<std::byte> Mlp::serialize() const {
    std::vector<std::byte> out;
    put(out, u32(m_layers.size()));
    for (const LayerDesc& l : m_layers) {
        put(out, l.inputs);
        put(out, l.outputs);
        put(out, u32(l.activation));
    }
    put(out, u64(m_params.size()));
    const auto* p = reinterpret_cast<const std::byte*>(m_params.data());
    out.insert(out.end(), p, p + m_params.size() * sizeof(f32));
    return out;
}

bool Mlp::deserialize(std::span<const std::byte> data, Mlp& out) {
    u32 n = 0;
    if (!get(data, n) || n == 0 || n > 64) return false;
    std::vector<LayerDesc> layers(n);
    for (LayerDesc& l : layers) {
        u32 act = 0;
        if (!get(data, l.inputs) || !get(data, l.outputs) || !get(data, act)) return false;
        if (l.inputs == 0 || l.outputs == 0 || l.inputs > 4096 || l.outputs > 4096 || act > 1) return false;
        l.activation = Activation(act);
    }
    for (u32 i = 1; i < n; ++i)
        if (layers[i].inputs != layers[i - 1].outputs) return false;
    Mlp m(std::move(layers));
    u64 count = 0;
    if (!get(data, count) || count != m.m_params.size() || data.size() < count * sizeof(f32)) return false;
    std::memcpy(m.m_params.data(), data.data(), count * sizeof(f32));
    m.syncTransposed();
    out = std::move(m);
    return true;
}

bool Mlp::operator==(const Mlp& o) const {
    if (m_layers.size() != o.m_layers.size() || m_params != o.m_params) return false;
    for (usize i = 0; i < m_layers.size(); ++i) {
        if (m_layers[i].inputs != o.m_layers[i].inputs || m_layers[i].outputs != o.m_layers[i].outputs ||
            m_layers[i].activation != o.m_layers[i].activation)
            return false;
    }
    return true;
}

// ---- worker pool ---------------------------------------------------------------------------------------------

struct WorkerPool::State {
    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable wake, done;
    const std::function<void(u32)>* job = nullptr;
    u32 count = 0;
    std::atomic<u32> next{0};
    u32 active = 0;
    u64 generation = 0;
    bool quit = false;

    void work() {
        for (;;) {
            const u32 i = next.fetch_add(1);
            if (i >= count) break;
            (*job)(i);
        }
    }
};

WorkerPool::WorkerPool(u32 threads) : m_state(std::make_unique<State>()) {
    if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
    State* s = m_state.get();
    for (u32 t = 1; t < threads; ++t) {
        s->threads.emplace_back([s] {
            ScopedFlushDenormals ftz;
            u64 seen = 0;
            for (;;) {
                {
                    std::unique_lock lock(s->mutex);
                    s->wake.wait(lock, [&] { return s->quit || s->generation != seen; });
                    if (s->quit) return;
                    seen = s->generation;
                }
                s->work();
                std::lock_guard lock(s->mutex);
                if (--s->active == 0) s->done.notify_all();
            }
        });
    }
}

WorkerPool::~WorkerPool() {
    {
        std::lock_guard lock(m_state->mutex);
        m_state->quit = true;
    }
    m_state->wake.notify_all();
    for (std::thread& t : m_state->threads) t.join();
}

u32 WorkerPool::threads() const { return u32(m_state->threads.size()) + 1; }

void WorkerPool::run(u32 count, const std::function<void(u32)>& fn) {
    State* s = m_state.get();
    if (count == 0) return;
    ScopedFlushDenormals ftz;
    if (s->threads.empty() || count == 1) {
        for (u32 i = 0; i < count; ++i) fn(i);
        return;
    }
    {
        std::lock_guard lock(s->mutex);
        s->job = &fn;
        s->count = count;
        s->next.store(0);
        s->active = u32(s->threads.size());
        ++s->generation;
    }
    s->wake.notify_all();
    s->work();
    std::unique_lock lock(s->mutex);
    s->done.wait(lock, [&] { return s->active == 0; });
    s->job = nullptr;
}

// ---- adam ----------------------------------------------------------------------------------------------------

void Adam::reset(usize count) {
    m_m.assign(count, 0.0f);
    m_v.assign(count, 0.0f);
    m_t = 0;
}

void Adam::step(std::span<f32> params, std::span<const f32> grads, f32 lr, const AdamParams& p, WorkerPool* pool) {
    OX_ASSERT(params.size() == m_m.size() && grads.size() == params.size(), "Adam: size mismatch");
    ++m_t;
    const f32 c1 = 1.0f / (1.0f - std::pow(p.beta1, f32(m_t)));
    const f32 c2 = 1.0f / (1.0f - std::pow(p.beta2, f32(m_t)));
    const f32 b1 = p.beta1, b2 = p.beta2, eps = p.epsilon;
    auto range = [&](usize begin, usize end) {
        f32* __restrict m = m_m.data();
        f32* __restrict v = m_v.data();
        f32* __restrict x = params.data();
        const f32* __restrict g = grads.data();
        for (usize i = begin; i < end; ++i) {
            m[i] = b1 * m[i] + (1.0f - b1) * g[i];
            v[i] = b2 * v[i] + (1.0f - b2) * g[i] * g[i];
            if (std::abs(m[i]) < 1e-30f) m[i] = 0.0f; // keep out of the denormal range
            if (v[i] < 1e-30f) v[i] = 0.0f;
            x[i] -= lr * (m[i] * c1) / (std::sqrt(v[i] * c2) + eps);
        }
    };
    const usize n = params.size();
    constexpr usize kChunk = 1u << 16;
    if (!pool || n < kChunk * 2) {
        range(0, n);
        return;
    }
    const u32 chunks = u32((n + kChunk - 1) / kChunk);
    pool->run(chunks, [&](u32 c) { range(usize(c) * kChunk, std::min(n, usize(c + 1) * kChunk)); });
}

// ---- generic regression ----------------------------------------------------------------------------------------

f32 meanSquaredError(const Mlp& net, std::span<const f32> inputs, std::span<const f32> targets, u32 count) {
    std::vector<f32> scratch(net.scratchSize()), out(net.outputs());
    f64 sum = 0.0;
    for (u32 s = 0; s < count; ++s) {
        net.forward(&inputs[usize(s) * net.inputs()], out.data(), scratch.data());
        for (u32 o = 0; o < net.outputs(); ++o) {
            const f64 d = f64(out[o]) - targets[usize(s) * net.outputs() + o];
            sum += d * d;
        }
    }
    return f32(sum / std::max<f64>(1.0, f64(count) * net.outputs()));
}

TrainResult trainRegression(Mlp& net, std::span<const f32> inputs, std::span<const f32> targets, u32 count,
                            const TrainOptions& o) {
    TrainResult r;
    Stopwatch sw(true);
    r.initialLoss = meanSquaredError(net, inputs, targets, count);
    WorkerPool pool(o.threads);
    constexpr u32 kChunk = 32;
    const u32 batch = std::max(o.batch, 1u);
    const u32 chunks = (batch + kChunk - 1) / kChunk;
    std::vector<std::vector<f32>> grads(chunks, std::vector<f32>(net.paramCount()));
    std::vector<Mlp::Tape> tapes(chunks);
    std::vector<f32> losses(chunks);
    std::vector<u32> indices(batch);
    std::vector<f32> total(net.paramCount());
    Adam adam;
    adam.reset(net.paramCount());
    Pcg32 rng(o.seed, 0x5851f42d4c957f2dull);
    const u32 nin = net.inputs(), nout = net.outputs();
    for (u32 step = 0; step < o.steps; ++step) {
        for (u32& i : indices) i = rng.below(count);
        pool.run(chunks, [&](u32 c) {
            const u32 begin = c * kChunk, n = std::min(batch, begin + kChunk) - begin;
            std::vector<f32> x(usize(n) * nin), d(usize(n) * nout);
            for (u32 s = 0; s < n; ++s)
                std::memcpy(&x[usize(s) * nin], &inputs[usize(indices[begin + s]) * nin], sizeof(f32) * nin);
            Mlp::Tape& tape = tapes[c];
            net.forwardTrain(x.data(), n, tape);
            const f32* y = net.tapeOutputs(tape);
            f32 loss = 0.0f;
            const f32 scale = 2.0f / f32(batch * nout);
            for (u32 s = 0; s < n; ++s) {
                for (u32 k = 0; k < nout; ++k) {
                    const f32 diff = y[usize(s) * nout + k] - targets[usize(indices[begin + s]) * nout + k];
                    loss += diff * diff;
                    d[usize(s) * nout + k] = diff * scale;
                }
            }
            losses[c] = loss;
            std::fill(grads[c].begin(), grads[c].end(), 0.0f);
            net.backward(tape, d.data(), grads[c].data());
        });
        std::fill(total.begin(), total.end(), 0.0f);
        f32 loss = 0.0f;
        for (u32 c = 0; c < chunks; ++c) {
            for (usize i = 0; i < total.size(); ++i) total[i] += grads[c][i];
            loss += losses[c];
        }
        const f32 t = f32(step) / f32(std::max(o.steps, 1u));
        const f32 lr = o.finalLearningRate +
                       0.5f * (o.learningRate - o.finalLearningRate) * (1.0f + std::cos(std::numbers::pi_v<f32> * t));
        adam.step(net.params(), total, lr);
        net.syncTransposed();
        if (o.lossEvery && step % o.lossEvery == 0) r.history.push_back(loss / f32(batch * nout));
    }
    r.finalLoss = meanSquaredError(net, inputs, targets, count);
    r.milliseconds = sw.elapsedMs();
    return r;
}

// ---- packing -------------------------------------------------------------------------------------------------

u32 padTo(u32 v, u32 multiple) { return (v + multiple - 1) / multiple * multiple; }

PackedNetwork packFp16(const Mlp& net, u32 padding) {
    PackedNetwork p;
    u32 cursor = 0;
    for (u32 li = 0; li < net.layers().size(); ++li) {
        const LayerDesc& l = net.layers()[li];
        PackedLayer pl;
        pl.inputs = l.inputs;
        pl.outputs = l.outputs;
        pl.inputsPadded = padTo(l.inputs, padding);
        pl.outputsPadded = padTo(l.outputs, padding);
        pl.activation = u32(l.activation);
        pl.weightOffset = cursor;
        cursor = padTo(cursor + pl.inputsPadded * pl.outputsPadded, 32);
        pl.biasOffset = cursor;
        cursor = padTo(cursor + pl.outputsPadded, 32);
        p.layers.push_back(pl);
    }
    p.halves.assign(cursor, 0);
    const auto params = net.params();
    for (u32 li = 0; li < net.layers().size(); ++li) {
        const PackedLayer& pl = p.layers[li];
        const f32* w = &params[net.weightOffset(li)];
        for (u32 o = 0; o < pl.outputs; ++o)
            for (u32 i = 0; i < pl.inputs; ++i)
                p.halves[pl.weightOffset + o * pl.inputsPadded + i] = floatToHalf(w[usize(o) * pl.inputs + i]);
        const f32* b = &params[net.biasOffset(li)];
        for (u32 o = 0; o < pl.outputs; ++o) p.halves[pl.biasOffset + o] = floatToHalf(b[o]);
    }
    return p;
}

} // namespace ox::nn
