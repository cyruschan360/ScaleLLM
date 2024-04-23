#pragma once
#include <curand_kernel.h>
#include <torch/torch.h>

namespace llm::kernel {

void apply_temperature_penalty(torch::Tensor& logits,
                               const torch::Tensor& temperatures);

// token_ids are unique token ids for each sequence.
// the order of token ids does not matter.
void apply_repetition_penalty(torch::Tensor& logits,
                              const torch::Tensor& token_ids,
                              const torch::Tensor& token_ids_lens,
                              const torch::Tensor& penalities);

// token_ids are unique token ids for each sequence.
// token_counts are the number of times corresponding token appears in the
// sequence.
void apply_frequency_presence_penalty(torch::Tensor& logits,
                                      const torch::Tensor& token_ids,
                                      const torch::Tensor& token_counts,
                                      const torch::Tensor& token_ids_lens,
                                      const torch::Tensor& frequency_penalties,
                                      const torch::Tensor& presence_penalties);

// calculate softmax in place
void invoke_softmax(torch::Tensor& logits);

void invoke_topk_sampling(torch::Tensor& output_ids,
                          torch::Tensor& output_log_probs,
                          torch::Tensor logits,
                          torch::Tensor workspace,
                          curandState_t* curandstate,
                          int max_top_k,
                          torch::Tensor top_ks,
                          torch::Tensor top_ps);

}  // namespace llm::kernel
