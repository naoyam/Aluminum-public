////////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2018, Lawrence Livermore National Security, LLC.  Produced at the
// Lawrence Livermore National Laboratory in collaboration with University of
// Illinois Urbana-Champaign.
//
// Written by the LBANN Research Team (N. Dryden, N. Maruyama, et al.) listed in
// the CONTRIBUTORS file. <lbann-dev@llnl.gov>
//
// LLNL-CODE-756777.
// All rights reserved.
//
// This file is part of Aluminum GPU-aware Communication Library. For details, see
// http://software.llnl.gov/Aluminum or https://github.com/LLNL/Aluminum.
//
// Licensed under the Apache License, Version 2.0 (the "Licensee"); you
// may not use this file except in compliance with the License.  You may
// obtain a copy of the License at:
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the license.
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "cuda.hpp"
#include "cuda_kernels.hpp"
#include "mpi_cuda/communicator.hpp"
#include "progress.hpp"

namespace Al {
namespace internal {
namespace mpi_cuda {

template <typename T>
class AlltoallAlState : public AlState {
public:
  AlltoallAlState(const T* sendbuf, T* recvbuf, size_t count,
                  MPICUDACommunicator& comm, cudaStream_t stream) :
    AlState(nullptr),
    host_mem_(get_pinned_memory<T>(comm.size()*count)),
    count_(count),
    comm_(comm.get_comm()),
    compute_stream(comm.get_stream()) {

    // Transfer data from device to host and use an event to determine when it
    // completes.
    AL_CHECK_CUDA(cudaMemcpyAsync(host_mem_, sendbuf, sizeof(T)*count*comm.size(),
                                  cudaMemcpyDeviceToHost, stream));
    d2h_event_.record(stream);

    // Enqueue the kernel to wait on the host
    gpuwait_.wait(stream);

    // Transfer completed buffer back to device.
    AL_CHECK_CUDA(cudaMemcpyAsync(recvbuf, host_mem_, sizeof(T)*count*comm.size(),
                                  cudaMemcpyHostToDevice, stream));
    h2d_event_.record(stream);
  }

  ~AlltoallAlState() override {
    release_pinned_memory(host_mem_);
  }

  bool step() override {
    if (!a2a_started_) {
      // Check if mem xfer complete
      if (d2h_event_.query()) {
        MPI_Ialltoall(MPI_IN_PLACE, count_, mpi::TypeMap<T>(),
                      host_mem_, count_, mpi::TypeMap<T>(), comm_, &req_);
        a2a_started_ = true;
      }
      else {
        return false;
      }
    }

    if (!a2a_done_) {
      // Wait for the all2all to complete
      int flag;
      MPI_Test(&req_, &flag, MPI_STATUS_IGNORE);
      if (flag) {
        a2a_done_ = true;
        gpuwait_.signal();
      }
      else {
        return false;
      }
    }

    // Wait for host-to-device memcopy; cleanup
    if (h2d_event_.query()) {
      return true;
    }

    return false;
  }

  bool needs_completion() const override { return false; }
  void* get_compute_stream() const override { return compute_stream; }

  std::string get_name() const override { return "HTAlltoall"; }

private:
  T* host_mem_;
  size_t count_;

  cuda::GPUWait gpuwait_;

  cuda::FastEvent d2h_event_, h2d_event_;

  MPI_Comm comm_;
  MPI_Request req_ = MPI_REQUEST_NULL;

  bool a2a_started_ = false;
  bool a2a_done_ = false;

  cudaStream_t compute_stream;
};

}  // namespace mpi_cuda
}  // namespace internal
}  // namespace Al
