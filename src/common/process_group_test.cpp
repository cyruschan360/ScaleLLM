#include "process_group.h"

#include <c10/core/Device.h>
#include <gtest/gtest.h>

namespace llm {

void all_reduce_test(std::vector<torch::Tensor>& tensors, ProcessGroup* pg) {
  const int rank = pg->rank();
  const int world_size = pg->world_size();
  const auto& device = pg->device();
  for (int i = 0; i <= tensors.size() - world_size; ++i) {
    auto tensor = tensors[i + rank].to(device);
    pg->allreduce(tensor);
    auto expected = torch::zeros_like(tensors[i]);
    for (int j = 0; j < world_size; ++j) {
      expected += tensors[i + j];
    }
    EXPECT_TRUE(torch::equal(tensor.cpu(), expected));
  }
}

void run_all_reduce(int world_size) {
  // create process groups
  std::vector<torch::Device> devices;
  devices.reserve(world_size);
  for (int i = 0; i < world_size; ++i) {
    devices.emplace_back(torch::kCUDA, i);
  }
  auto process_groups = ProcessGroup::create_process_groups(devices);
  EXPECT_EQ(process_groups.size(), world_size);

  // create tensors
  const int num_test_tensors = 50;
  std::vector<torch::Tensor> tensors;
  tensors.reserve(num_test_tensors);
  for (int i = 0; i < num_test_tensors; ++i) {
    tensors.push_back(torch::ones({100, 4096}, torch::kHalf));
  }

  // run all reduce
  std::vector<std::thread> threads;
  threads.reserve(process_groups.size());
  for (int i = 0; i < world_size; ++i) {
    threads.emplace_back([&tensors, pg = process_groups[i].get()]() {
      all_reduce_test(tensors, pg);
    });
  }

  // wait for all threads to finish
  for (auto& thread : threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }
}

TEST(ProcessGroupTest, NCCLAllReduce) {
  // skip test if less than two gpus
  for (int i = 2; i <= torch::cuda::device_count(); i += 2) {
    run_all_reduce(i);
  }
}

}  // namespace llm