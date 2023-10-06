#pragma once

#include <torch/torch.h>

#include "layers/activation.h"
#include "layers/attention.h"
#include "layers/embedding.h"
#include "layers/linear.h"
#include "layers/normalization.h"
#include "layers/pos_embedding.h"
#include "memory/kv_cache.h"
#include "models/args.h"
#include "models/input_parameters.h"

// Aquila model compatible with huggingface weights
namespace llm::hf {

class AquilaMLPImpl : public torch::nn::Module {
 public:
  AquilaMLPImpl(const ModelArgs& args,
                const QuantizationArgs& quant_args,
                const ParallelArgs& parallel_args,
                torch::ScalarType dtype,
                const torch::Device& device) {
    act_ = Activation::get("silu", device);
    CHECK(act_ != nullptr);

    const int64_t hidden_size = args.hidden_size();
    const int64_t intermediate_size = args.intermediate_size();

    // register the weight parameter
    gate_up_proj_ =
        register_module("gate_up_proj",
                        ColumnParallelLinear(hidden_size,
                                             intermediate_size * 2,
                                             /*bias=*/false,
                                             /*gather_output=*/false,
                                             quant_args,
                                             parallel_args,
                                             dtype,
                                             device));
    down_proj_ =
        register_module("down_proj",
                        RowParallelLinear(intermediate_size,
                                          hidden_size,
                                          /*bias=*/false,
                                          /*input_is_parallelized=*/true,
                                          quant_args,
                                          parallel_args,
                                          dtype,
                                          device));
  }

  torch::Tensor forward(torch::Tensor x) {
    auto gate_up_proj = gate_up_proj_(x);
    auto chunks = gate_up_proj.chunk(/*chunks=*/2, /*dim=*/-1);
    return down_proj_(act_(chunks[0]) * chunks[1]);
  }

  // load the weight from the checkpoint
  void load_state_dict(const StateDict& state_dict) {
    // call each submodule's load_state_dict function
    gate_up_proj_->load_state_dict(state_dict, {"gate_proj.", "up_proj."});
    down_proj_->load_state_dict(state_dict.select("down_proj."));
  }

  void verify_loaded_weights(const std::string& prefix) const {
    gate_up_proj_->verify_loaded_weights(prefix + "[gate_proj,up_proj].");
    down_proj_->verify_loaded_weights(prefix + "down_proj.");
  }

 private:
  // parameter members, must be registered
  ColumnParallelLinear gate_up_proj_{nullptr};
  RowParallelLinear down_proj_{nullptr};

  ActFunc act_{nullptr};
};
TORCH_MODULE(AquilaMLP);

class AquilaAttentionImpl : public torch::nn::Module {
 public:
  AquilaAttentionImpl(const ModelArgs& args,
                      const QuantizationArgs& quant_args,
                      const ParallelArgs& parallel_args,
                      torch::ScalarType dtype,
                      const torch::Device& device)
      : parallel_args_(parallel_args) {
    const auto world_size = parallel_args.world_size();
    const int64_t hidden_size = args.hidden_size();
    const int64_t n_heads = args.n_heads();
    const int64_t n_kv_heads = args.n_kv_heads().value_or(n_heads);

    n_local_heads_ = n_heads / world_size;
    n_local_kv_heads_ = n_kv_heads / world_size;
    head_dim_ = hidden_size / n_heads;
    // size for q, k, v
    qkv_sizes_ = {n_local_heads_ * head_dim_,
                  n_local_kv_heads_ * head_dim_,
                  n_local_kv_heads_ * head_dim_};

    // register submodules
    qkv_proj_ = register_module(
        "qkv_proj",
        ColumnParallelLinear(hidden_size,
                             (n_heads + 2 * n_kv_heads) * head_dim_,
                             /*bias=*/false,
                             /*gather_output=*/false,
                             quant_args,
                             parallel_args,
                             dtype,
                             device));

    o_proj_ = register_module("o_proj",
                              RowParallelLinear(hidden_size,
                                                hidden_size,
                                                /*bias=*/false,
                                                /*input_is_parallelized=*/true,
                                                quant_args,
                                                parallel_args,
                                                dtype,
                                                device));

    // initialize attention
    const float scale = 1.0f / std::sqrt(static_cast<float>(head_dim_));
    atten_ = register_module("atten",
                             AttentionWithRoPE(n_local_heads_,
                                               n_local_kv_heads_,
                                               head_dim_,
                                               scale,
                                               /*rotary_dim=*/head_dim_,
                                               args.rope_scaling(),
                                               args.rope_theta(),
                                               args.max_position_embeddings(),
                                               /*interleaved=*/false,
                                               dtype,
                                               device));
  }

  torch::Tensor forward(torch::Tensor x,
                        torch::Tensor positions,
                        KVCache& kv_cache,
                        const InputParameters& input_params) {
    const auto num_tokens = x.size(0);
    // (num_tokens, dim) x (dim, n_local_heads * head_dim)
    // => (num_tokens, n_local_heads * head_dim)
    auto qkv = qkv_proj_(x).split(/*split_size=*/qkv_sizes_, /*dim=*/-1);
    DCHECK_EQ(qkv.size(), 3);

    // calculate attention, output: (num_tokens, n_local_heads * head_dim)
    auto output =
        atten_(qkv[0], qkv[1], qkv[2], positions, kv_cache, input_params);
    return o_proj_(output);
  }

  // load the weight from the checkpoint
  void load_state_dict(const StateDict& state_dict) {
    // call each submodule's load_state_dict function
    qkv_proj_->load_state_dict(state_dict, {"q_proj.", "k_proj.", "v_proj."});
    o_proj_->load_state_dict(state_dict.select("o_proj."));
  }

  void verify_loaded_weights(const std::string& prefix) const {
    qkv_proj_->verify_loaded_weights(prefix + "[q_proj,k_proj,v_proj].");
    o_proj_->verify_loaded_weights(prefix + "o_proj.");
  }

 private:
  // parameter members, must be registered
  ColumnParallelLinear qkv_proj_{nullptr};

  RowParallelLinear o_proj_{nullptr};

  // module members without parameters
  AttentionWithRoPE atten_{nullptr};

  ParallelArgs parallel_args_;

  // configs used in forward
  int64_t n_local_heads_;
  int64_t n_local_kv_heads_;
  int64_t head_dim_;

  // size for q, k, v
  std::vector<int64_t> qkv_sizes_;
};
TORCH_MODULE(AquilaAttention);

class AquilaDecoderLayerImpl : public torch::nn::Module {
 public:
  AquilaDecoderLayerImpl(const ModelArgs& args,
                         const QuantizationArgs& quant_args,
                         const ParallelArgs& parallel_args,
                         torch::ScalarType dtype,
                         const torch::Device& device)
      : parallel_args_(parallel_args) {
    // register submodules
    self_attn_ = register_module(
        "self_attn",
        AquilaAttention(args, quant_args, parallel_args, dtype, device));
    mlp_ = register_module(
        "mlp", AquilaMLP(args, quant_args, parallel_args, dtype, device));
    input_layernorm_ = register_module(
        "input_layernorm",
        RMSNorm(args.hidden_size(), args.rms_norm_eps(), dtype, device));
    post_attention_layernorm_ = register_module(
        "post_attention_layernorm",
        RMSNorm(args.hidden_size(), args.rms_norm_eps(), dtype, device));
  }

  torch::Tensor forward(torch::Tensor x,
                        torch::Tensor positions,
                        KVCache& kv_cache,
                        const InputParameters& input_params) {
    auto h =
        x + self_attn_(input_layernorm_(x), positions, kv_cache, input_params);
    return h + mlp_(post_attention_layernorm_(h));
  }

  // load the weight from the checkpoint
  void load_state_dict(const StateDict& state_dict) {
    // call each submodule's load_state_dict function
    self_attn_->load_state_dict(state_dict.select("self_attn."));
    mlp_->load_state_dict(state_dict.select("mlp."));
    input_layernorm_->load_state_dict(state_dict.select("input_layernorm."));
    post_attention_layernorm_->load_state_dict(
        state_dict.select("post_attention_layernorm."));
  }

  void verify_loaded_weights(const std::string& prefix) const {
    self_attn_->verify_loaded_weights(prefix + "self_attn.");
    mlp_->verify_loaded_weights(prefix + "mlp.");
    input_layernorm_->verify_loaded_weights(prefix + "input_layernorm.");
    post_attention_layernorm_->verify_loaded_weights(
        prefix + "post_attention_layernorm.");
  }

 private:
  // parameter members, must be registered
  AquilaAttention self_attn_{nullptr};

  AquilaMLP mlp_{nullptr};

  RMSNorm input_layernorm_{nullptr};

  RMSNorm post_attention_layernorm_{nullptr};

  ParallelArgs parallel_args_;
};
TORCH_MODULE(AquilaDecoderLayer);

class AquilaModelImpl : public torch::nn::Module {
 public:
  AquilaModelImpl(const ModelArgs& args,
                  const QuantizationArgs& quant_args,
                  const ParallelArgs& parallel_args,
                  torch::ScalarType dtype,
                  const torch::Device& device)
      : parallel_args_(parallel_args) {
    // register submodules
    embed_tokens_ = register_module("embed_tokens",
                                    ParallelEmbedding(args.vocab_size(),
                                                      args.hidden_size(),
                                                      parallel_args,
                                                      dtype,
                                                      device));
    blocks_ = register_module("layers", torch::nn::ModuleList());
    layers_.reserve(args.n_layers());
    for (int32_t i = 0; i < args.n_layers(); i++) {
      auto block =
          AquilaDecoderLayer(args, quant_args, parallel_args, dtype, device);
      layers_.push_back(block);
      blocks_->push_back(block);
    }
    norm_ = register_module(
        "norm",
        RMSNorm(args.hidden_size(), args.rms_norm_eps(), dtype, device));

    lm_head_ = register_module("lm_head",
                               ColumnParallelLinear(args.hidden_size(),
                                                    args.vocab_size(),
                                                    /*bias=*/false,
                                                    /*gather_output=*/true,
                                                    parallel_args,
                                                    dtype,
                                                    device));
  }

  // tokens: [num_tokens]
  // positions: [num_tokens] token pos in the sequence
  torch::Tensor forward(torch::Tensor tokens,
                        torch::Tensor positions,
                        std::vector<KVCache>& kv_caches,
                        const InputParameters& input_params) {
    auto h = embed_tokens_(tokens);
    for (size_t i = 0; i < layers_.size(); i++) {
      auto& layer = layers_[i];
      h = layer(h, positions, kv_caches[i], input_params);
    }
    h = norm_(h);
    // select last token for each sequence
    h = h.index_select(/*dim=*/0, input_params.last_token_indicies);
    return lm_head_(h);
  }

  // load the weight from the checkpoint
  void load_state_dict(const StateDict& state_dict) {
    embed_tokens_->load_state_dict(state_dict.select("model.embed_tokens."));
    // call each layer's load_state_dict function
    for (int i = 0; i < layers_.size(); i++) {
      layers_[i]->load_state_dict(
          state_dict.select("model.layers." + std::to_string(i) + "."));
    }
    norm_->load_state_dict(state_dict.select("model.norm."));
    lm_head_->load_state_dict(state_dict.select("lm_head."));
  }

  void verify_loaded_weights() const {
    embed_tokens_->verify_loaded_weights("model.embed_tokens.");
    for (int i = 0; i < layers_.size(); i++) {
      layers_[i]->verify_loaded_weights("model.layers." + std::to_string(i) +
                                        ".");
    }
    norm_->verify_loaded_weights("model.norm.");
    lm_head_->verify_loaded_weights("lm_head.");
  }

 private:
  // parameter members, must be registered
  ParallelEmbedding embed_tokens_{nullptr};

  torch::nn::ModuleList blocks_{nullptr};
  // hold same data but different type as blocks_ to avoid type cast
  std::vector<AquilaDecoderLayer> layers_;

  RMSNorm norm_{nullptr};

  ColumnParallelLinear lm_head_{nullptr};

  ParallelArgs parallel_args_;
};
TORCH_MODULE(AquilaModel);

}  // namespace llm::hf