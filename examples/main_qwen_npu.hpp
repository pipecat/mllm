#include <cmath>
#include <csignal>
#include "Types.hpp"
#include "express/Express.hpp"

using namespace mllm;

namespace modeling {

NetTensor *Qwen_FFN_NPU(Context *c, NetTensor *i, int hidden_dim, int ffn_hidden_dim, string name) {
    auto *x = _LinearINT8({i}, hidden_dim, ffn_hidden_dim, false, name + ".gate_proj");
    auto *y = _LinearINT8({i}, hidden_dim, ffn_hidden_dim, false, name + ".up_proj");
    x = _SuperSiLU({x,y}, name + ".supersilu");
    // x = _Dequantize({x}, true, (string)name + ".gate_proj.dequantize", false);
    // y = _Dequantize({y}, true, (string)name + ".up_proj.dequantize", false);
    // x = _SiLU({x}, name + ".silu");
    // x = *x * y;
    // x = _Quantize({x}, true, (string)name + ".down_proj.quantize");
    x = _LinearINT8({x}, ffn_hidden_dim, hidden_dim, false, name + ".down_proj");
    x = _Dequantize({x}, true, (string)name + ".down_proj.dequantize");
    return x;
}

std::vector<NetTensor *> Qwen_CPUNPUAttention(Context *c, NetTensor *x, NetTensor *res, int embedding_size, int hidden_size, int head_size, int cache_max, string name, int seq, int chunk) {
    x = x->view(1, static_cast<int>(seq / chunk / 32), static_cast<int>(32), hidden_size * head_size);
    auto *q = _LinearINT8({x}, embedding_size, hidden_size * head_size, true, name + ".q_proj");
    auto *k = _LinearINT8({x}, embedding_size, hidden_size * head_size, true, name + ".k_proj");
    auto *v = _LinearINT8({x}, embedding_size, hidden_size * head_size, true, name + ".v_proj");
    q = q->view(1, head_size, seq / chunk, hidden_size);
    k = k->view(1, head_size, seq / chunk, hidden_size);
    v = v->view(1, head_size, seq / chunk, hidden_size);

    q = _Dequantize({q}, true, (string)name + ".q_proj.dequantize", true);
    k = _Dequantize({k}, true, (string)name + ".k_proj.dequantize", false);
    v = _Dequantize({v}, true, (string)name + ".v_proj.dequantize", false);

    v = _Transpose({v}, {0, 2, 3, 1}, (string)name + ".v_proj.transpose");

    auto *m = _MergeOutput({q, k, v, res}, name + ".qkv_merge");

    // --------------------
    _SubgraphBegin(c, MLLM_CPU);
    // --------------------

    auto s = _SplitInput({m}, true, 4, name + ".qkv_split");

    q = s[0];
    k = s[1];
    v = s[2];
    res = s[3];

    q = _RoPE({q}, HFHUBROPE, name + ".q_rope", 1000000, 32768);
    k = _RoPE({k}, HFHUBROPE, name + ".k_rope", 1000000, 32768);

    k = _KVCacheNPU({k}, cache_max, name + ".k_cache");
    v = _KVCacheNPU({v}, cache_max, name + ".v_cache");

    auto *qk = _Matmul({q, k}, false, true, name + ".qk");
    // qk = *qk / std::sqrt(hidden_size);
    // qk = _Causalmask({qk}, name + ".mask");
    qk = _Softmax({qk}, DIMENSION, true, name + ".softmax");

    auto *o = _Matmul({qk, v}, false, false, name + ".qkv");
    o = _Quantize({o}, true, (string)name + ".o_proj.quantize");
    m = _MergeOutput({o, res}, name + ".or_merge");

    // --------------------
    _SubgraphBegin(c);
    // --------------------
    s = _SplitInput({m}, true, 2, name + ".or_split");

    o = s[0];
    res = s[1];

    o = o->view(1, static_cast<int>(seq / chunk / 32), static_cast<int>(32), hidden_size * head_size);
    res = res->view(-1, 1, -1, hidden_size * head_size);
    o = _LinearINT8({o}, hidden_size * head_size, embedding_size, false, name + ".o_proj");
    o = _Dequantize({o}, true, (string)name + ".o_proj.dequantize");

    return {o, res};
}

NetTensor *Qwen_CPUAttention(Context *c, NetTensor *x, int embedding_size, int hidden_size, int head_size, int cache_max, string name, int seq, int chunk) {
    auto *q = _LinearINT8({x}, embedding_size, hidden_size * head_size, true, name + ".q_proj");
    auto *k = _LinearINT8({x}, embedding_size, hidden_size * head_size, true, name + ".k_proj");
    auto *v = _LinearINT8({x}, embedding_size, hidden_size * head_size, true, name + ".v_proj");
    q = q->view(-1, head_size, -1, hidden_size);
    k = k->view(-1, head_size, -1, hidden_size);
    v = v->view(-1, head_size, -1, hidden_size);

    q = _RoPE({q}, HFHUBROPE, name + ".q_rope", 1000000, 32768);
    k = _RoPE({k}, HFHUBROPE, name + ".k_rope", 1000000, 32768);

    k = _KVCacheNPU({k}, cache_max, name + ".k_cache");
    v = _KVCacheNPU({v}, cache_max, name + ".v_cache");

    auto *qk = _Matmul({q, k}, false, true, name + ".qk");
    qk = *qk / std::sqrt(hidden_size);
    // qk = _Causalmask({qk}, name + ".mask");
    qk = _Softmax({qk}, DIMENSION, true, name + ".softmax");

    auto *o = _Matmul({qk, v}, false, false, name + ".qkv");

    o = o->view(-1, 1, -1, hidden_size * head_size);

    o = _LinearINT8({o}, hidden_size * head_size, embedding_size, false, name + ".o_proj");

    return o;
}

NetTensor *Qwen_CPUAttention_q4k(Context *c, NetTensor *x, int embedding_size, int hidden_size, int head_size, int cache_max, string name, int seq, int chunk) {
    auto *q = _Linear({x}, embedding_size, hidden_size * head_size, true, name + ".q_proj");
    auto *k = _Linear({x}, embedding_size, hidden_size * head_size, true, name + ".k_proj");
    auto *v = _Linear({x}, embedding_size, hidden_size * head_size, true, name + ".v_proj");
    q = q->view(-1, head_size, -1, hidden_size);
    k = k->view(-1, head_size, -1, hidden_size);
    v = v->view(-1, head_size, -1, hidden_size);

    q = _RoPE({q}, HFHUBROPE, name + ".q_rope", 1000000, 32768);
    k = _RoPE({k}, HFHUBROPE, name + ".k_rope", 1000000, 32768);

    k = _KVCacheNPU({k}, cache_max, name + ".k_cache");
    v = _KVCacheNPU({v}, cache_max, name + ".v_cache");

    auto *qk = _Matmul({q, k}, false, true, name + ".qk");
    qk = *qk / std::sqrt(hidden_size);
    // qk = _Causalmask({qk}, name + ".mask");
    qk = _Softmax({qk}, DIMENSION, true, name + ".softmax");

    auto *o = _Matmul({qk, v}, false, false, name + ".qkv");

    o = o->view(-1, 1, -1, hidden_size * head_size);

    o = _Linear({o}, hidden_size * head_size, embedding_size, false, name + ".o_proj");

    return o;
}

NetTensor *Qwen_FFN_CPU(Context *c, NetTensor *i, int hidden_dim, int ffn_hidden_dim, string name) {
    auto *x = _LinearINT8({i}, hidden_dim, ffn_hidden_dim, false, name + ".gate_proj");
    auto *y = _LinearINT8({i}, hidden_dim, ffn_hidden_dim, false, name + ".up_proj");
    x = _SiLU({x}, name + ".silu");
    x = *x * y;
    x = _LinearINT8({x}, ffn_hidden_dim, hidden_dim, false, name + ".down_proj");
    return x;
}

NetTensor *Qwen_FFN_CPU_q4k(Context *c, NetTensor *i, int hidden_dim, int ffn_hidden_dim, string name) {
    auto *x = _Linear({i}, hidden_dim, ffn_hidden_dim, false, name + ".gate_proj");
    auto *y = _Linear({i}, hidden_dim, ffn_hidden_dim, false, name + ".up_proj");
    x = _SiLU({x}, name + ".silu");
    x = *x * y;
    x = _Linear({x}, ffn_hidden_dim, hidden_dim, false, name + ".down_proj");
    return x;
}

void qwen_cpu_q4k(Context *c, int vocab_size = 32000, int hidden_dim = 4096, int ffn_hidden_dim = 11008, int mutil_head_size = 32, int cache_max = 200, int seq = 256, int chunk = 2) {
    auto *i = _Input(c);
    i = _Embedding({i}, vocab_size, hidden_dim, (string) "model.embed_tokens");

    for (int layer = 0; layer < 24; ++layer) {
        auto res = _RMSNorm({i}, hidden_dim, 1e-6, (string) "model.layers." + std::to_string(layer) + ".input_layernorm");

        i = *Qwen_CPUAttention_q4k(c, res, hidden_dim, hidden_dim / mutil_head_size, mutil_head_size, cache_max, (string) "model.layers." + std::to_string(layer) + ".self_attn", seq, chunk) + i;

        res = _RMSNorm({i}, hidden_dim, 1e-6, (string) "model.layers." + std::to_string(layer) + ".post_attention_layernorm");

        i = *Qwen_FFN_CPU_q4k(c, res, hidden_dim, ffn_hidden_dim, (string) "model.layers." + std::to_string(layer) + ".mlp") + i;
    }
    i = _RMSNorm({i}, hidden_dim, 1e-6, (string) "model.norm");
    i = _Linear({i}, hidden_dim, vocab_size, false, "lm_head");
}

// merge o and FFN.
void qwen_npu(Context *c, int vocab_size = 32000, int hidden_dim = 4096, int ffn_hidden_dim = 11008, int mutil_head_size = 32, int cache_max = 200, int seq = 256, int chunk = 2) {
    auto *i = _Input(c);
    i = _Embedding({i}, vocab_size, hidden_dim, (string) "model.embed_tokens");

    // first 23 layer using NPU-CPU prefilling
    for (int layer = 0; layer < 24; ++layer) {

        auto res = i;
        res = res->view(-1, mutil_head_size, -1, hidden_dim / mutil_head_size, (layer != 0));

        i = _RMSNorm({i}, hidden_dim, 1e-6, (string) "model.layers." + std::to_string(layer) + ".input_layernorm");
        i = _Quantize({i}, true, (string) "model.layers." + std::to_string(layer) + ".self_attn.q_proj.quantize");        

        // only CPU graphs need merge and split
        if (layer == 7 || layer == 2 || layer == 3 || layer == 0) {
        // SHADOW
        // if (layer == 2 || layer == 0) {
            i = i->view(-1, mutil_head_size, -1, hidden_dim / mutil_head_size);

            auto *m = _MergeOutput({i, res}, (string) "model.layers." + std::to_string(layer) + ".ires_merge");
            
            _SubgraphBegin(c);
            auto s = _SplitInput({m}, true, 2, (string) "model.layers." + std::to_string(layer) + ".self_attn.ires_split");

            i = s[0];
            res = s[1];
        }
        
        auto ix = Qwen_CPUNPUAttention(c, i, res, hidden_dim, hidden_dim / mutil_head_size, mutil_head_size, cache_max, (string) "model.layers." + std::to_string(layer) + ".self_attn", seq, chunk);

        i = ix[0];
        res = ix[1];

        i = i->view(1, 1, seq / chunk, hidden_dim);
        i = *i + res;

        res = i;

        i = _RMSNorm({i}, hidden_dim, 1e-6, (string) "model.layers." + std::to_string(layer) + ".post_attention_layernorm", false);

        // i = _Quantize({i}, true, (string) "model.layers." + std::to_string(layer) + ".mlp.up_proj.quantize");

        i = i->view(1, static_cast<int>(seq / chunk / 32), static_cast<int>(32), hidden_dim);

        // SHADOW
        if (layer != 6 && layer != 1 && layer != 2) {
        // if (layer != 1) {
            i = Qwen_FFN_NPU(c, i, hidden_dim, ffn_hidden_dim, (string) "model.layers." + std::to_string(layer) + ".mlp");

            i = i->view(1, 1, seq / chunk, hidden_dim);

            i = *i + res;

        } else {
            auto name = (string) "model.layers." + std::to_string(layer) + ".mlp";
            auto *x = _LinearINT8({i}, hidden_dim, ffn_hidden_dim, false, name + ".gate_proj");
            auto *y = _LinearINT8({i}, hidden_dim, ffn_hidden_dim, false, name + ".up_proj");
            x = _Dequantize({x}, true, (string)name + ".gate_proj.dequantize", true);
            y = _Dequantize({y}, true, (string)name + ".up_proj.dequantize", true);
            x = _SiLU({x}, name + ".silu");
            x = *x * y;

            auto *i1 = x;
            x = _Quantize({x}, true, (string)name + ".down_proj.quantize");

            x = _LinearINT8({x}, ffn_hidden_dim, hidden_dim, false, name + ".down_proj");

            auto *i2 = x;
            x = _Dequantize({x}, true, (string)name + ".down_proj.dequantize");

            x = x->view(1, 1, seq / chunk, hidden_dim);

            x = *x + res;

            auto shadow = _LinearINT8ShadowMerge({i1, i2, x}, ffn_hidden_dim, hidden_dim, false, name + ".down_proj.shadow.qnn");

            _SubgraphBegin(c, MLLM_CPU);
            i = _LinearINT8ShadowCPU(shadow, ffn_hidden_dim, hidden_dim, false, name + ".down_proj.shadow");
        }
    }
}

void qwen_npu_cpu_inter(Context *c, int vocab_size = 32000, int hidden_dim = 4096, int ffn_hidden_dim = 11008, int mutil_head_size = 32, int cache_max = 200, int seq = 256, int chunk = 2) {
    auto *i = _Input(c);
    i = _RMSNorm({i}, hidden_dim, 1e-6, (string) "model.norm");
    i = _Linear({i}, hidden_dim, vocab_size, false, "lm_head");
}
} // namespace modeling
